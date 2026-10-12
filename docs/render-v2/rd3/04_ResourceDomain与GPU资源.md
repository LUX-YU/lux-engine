# 04 — ResourceDomain、GPU 资产身份、Residency 与数据流

## 4.1 Domain 不是一个新的资源管理器层次

公共 `ResourceDomain` 是 Backend-managed、生成式 ID、parent ID 与单个 `ResourceRegistry` 的小型 owner 记录；`ResourceRegistry` 是唯一的类型化资源容器。典型树：Root（共享 Shader/Texture/Mesh/Material 资产存储）、Scene（Light/Transform/Scene Buffer）、View（Camera/History/局部 Surface 数据）。允许具体调用者显式建立其他子域，**不依据资源 C++ T 编译时锁死 Global/Scene/View**。

```cpp
// 仅 Backend owner-thread 中的目标 API
ResourceRegistry& r = backend.resources(view.domain());
auto& camera = r.get<CameraBuffer>();           // 最近匹配：View
auto& lights = r.get<LightBuffer>();           // 祖先 Scene
auto& meshes = r.get<MeshStorage>();           // 祖先 Root
auto& mine = r.getLocal<CameraBuffer>();
// 写操作要求在实际拥有该资源的 Domain 上获得授权，不能通过透明父链写入。
```

同 Domain 同 Type+Key 的歧义必须被拒绝；当前 Domain 若遮蔽祖先同 Type，需固定明确定义的最近查找规则或 cold reject，不能依赖插入顺序。Client 绝不能直接持 Backend Registry 引用：跨线程只能通过 R2 typed protocol 和 Session handles。

## 4.2 Domain 与 GPU 物理存储

Domain 负责：资源可见性/归属、撤销新访问、logical generation、budget accounting 和本域 Registry 的析构事务。Backend Native Buffer/Image/Arena 负责：真实分配、capacity/offset/layout、GPU ready/last-use、descriptor backing versions、physical retire。**ResourceDomainId 绝不代替 VkAllocation lifetime**；是否缓存最后 release 的资产由 CPU 策略和 Backend 预算将来协商，不得自动强制 LRU。

CPU 资产 `AssetId`、Client GPU handle `GpuResourceId`、Native VkBuffer/VkImage 和 Graph 局部 `GraphResourceId` 四者不同。业务 retain/release 是逻辑合法性；缓存还在不允许旧 generation resurrect；GPU 使用完之前，即便逻辑已 release，也禁止覆写/释放旧物理 byte range。

## 4.3 数据更新不复制 ECS World

```text
Engine ECS/Assets (CPU single authority)
  └─ change events + generation + data packing
       └─ RenderSystem delta projection (O(D))
            └─ PROGRAM/UPLOAD
                 └─ Backend ResourceDomain's GPU-resident storage
                       Native buffers/images, versions, ready, descriptors
```

Backend 必须能保存必要的 GPU 容量/slot ID/上传 staging/packing metadata，但不能再建立一份所有 ECS Components 的常驻 CPU Store。Mesh large vertices、Texture pixels 可以仅在上传 admission 时暂持 owner-pinned bytes；若 Device Lost 需要重供，应由资产系统重新提供。GPU-authored `Particle/History/IndirectArgs` 不需要 CPU 读回才被下一 Pass 使用。

## 4.4 物理布局由消费需求驱动

| 工作负载 | 适用物理布局 | 必须验证 |
|---|---|---|
| 频繁零散 Light/Instance 增量 | stable slots + dirty range/packed scatter | O(D) CPU 构造、GPU gather 成本、无旧 slot 覆写 |
| Dense linear GPU traversal | dense Buffer/active ID workset | 重排时 Generation/Native 借用正确 |
| Mesh/PointCloud/Gaussian streaming | chunk/arena/page + GPU workset | GPU residency、LOD/fallback、峰值显存、异步 Ready |
| Texture/3D image/cubemap | Native Image + view/sampler contracts | subresource layout/format/usage/alias |
| HZB/TSR/TAA history | View-dependent persistent image versions | resize/cut/epoch/target change 与上一帧票据 |
| Cull scratch/DrawArgs 临时 | GraphExecutionContext 中 Graph transient | Pass live intervals、GPU Queue overlap、别名证明 |

DataSchema 的 GPU ABI 不因 Native Compiler 为省内存随意改变 AoS→SoA。需要专门访问模式必须先选择 ABI/生成 Shader accessor，再生成相容 Native Graph 变体。Shader 热路径不做按数据类型的动态 map/hash 查询。

## 4.5 ResourceDomain 的安装、移除与热替换

- `createDomain(parent)` 冷态事务，生成稳定身份；先验证 parent/Backend owner 及容量，再发布。
- Domain 将废弃时先撤销新 lease、处理所有子域，进入逻辑不可见状态；旧 Native GPU owners 由 last-use/Presentation 的完成证据延迟销毁。
- GPU backing resize/new version：创建完整候选分配、copy/upload/Ready→构造 bindings→发布新 version；旧 GPU reader 未完成前不能回收物理存储。
- Schema/Binding ABI 改动需要新 Shader/PSO/Graph；仅同 ABI 的资源实例变更通常只更新执行 Context binding，不重新编译。
- 同一 Mesh 被 Scene A/B 使用：Root 资源一份、两 Scene 各自记录 Mesh instance/Transform，资产引用计数独立于最后 GPU Ticket。

## 4.6 状态与回执粒度

建议区分 `observed/accepted/applied/gpu_ready`；Backpressure 不清原始 Dirty，合并连续更新只在未接纳且不丢 create/delete/generation 语义时允许。`SubmissionTicket` 不是 revision。测试包括 10k/100k/1M records、D=0/0.1/1/10/100%、clustered/random、Scene A/B 同一 Asset、多 View/2–3 FIF、GPU-only 路径和 fault-injected resize/retire。

## 4.7 不是“TypedStorage 全面禁用”

若 render backend **确实**自己生产了 CPU-authoritative 非 ECS 数据，局部 `TypedStorage<T>` 作为普通 C++ 容器算法是允许的；但它不是系统中心的第二 ECS，也不因所有 GPU 资源具有类型 Schema 就自动创建。业务权威来源必须在每种真实消费者的设计中标明。Shader/Native 背景所有权仍只有一个 Backend。
