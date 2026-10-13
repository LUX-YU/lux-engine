# 31 — Mesh/Texture 大资源专用上传：三种队列、CPU/GPU 拥有权与真实 Ready Ticket

> **不能把三种 Queue 说成一回事**：① Engine→Server 的协议 `UPLOAD` Lane；② Server 内部 CPU Job/Completion Queue；③ Vulkan Transfer/Graphics `VkQueue`。它们的 producer、backpressure、生命周期和同步不同。Legacy 已有优化过的三层实现，而当前 P2-A 只保留第一层的 Transport 机制，Mesh/Texture 上传业务尚未接线。

## 31.1 已核对源码与关键机制

| 层 | 源文件 | 当前/Legacy 事实 |
|---|---|---|
| 新 R2 Transport | `modules/function/render/transport/include/lux/engine/render/transport/{Transport.hpp,Operation.hpp}` | PROGRAM/CONTROL/UPLOAD 三 Lane 与 packet/reply arena/admission，默认 `pending_uploads=64`, `upload_bytes=256 MiB`；这些是默认预算，不是吞吐资格 |
| 新 P2-A Runtime | `modules/function/render/runtime/src/Backend.cpp` | `UPLOAD` 目前只在 shutdown drain 时 `pollUpload`；**尚无 Mesh/Texture active upload consumer** |
| Legacy Client | `render_legacy/modules/function/render/client/include/lux/engine/function/render/client/{RenderUploadClient.hpp,RenderUploadSession.hpp}` | 线程安全 producer 封装、拥有数据的 PreparedUpload、请求/回复、队列 full/byte budget/stopping，coordinator SPSC 所有权 |
| Legacy Host | `render_legacy/modules/function/render/runtime/pinclude/lux/engine/render/detail/UploadQueue.hpp` | bounded MPMC enqueue with byte accounting → coordinator submit，有明确 backpressure，按请求序处理 |
| Legacy Native Transfer | `render_legacy/modules/function/render/vulkan/{sinclude/lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp,src/resources/lifecycle/GpuTransferPipeline.cpp}` | 仅 Mesh/Texture/Cube 的 typed jobs；一条 GPU transfer worker + SPSC jobs/results，支持 DEDICATED_QUEUE/RECORD_ONLY、部分 submit 与 stop/drain |
| Legacy Staging | `render_legacy/modules/function/render/vulkan/sinclude/lux/engine/render/gpu/transfer/TransferScheduler.hpp` | staging ring、overflow FIF reuse、coalesced Buffer/Image copies 与 pre/post barriers、贡献者 hook |
| 当前 R4 Queue | `modules/function/render/vulkan/include/lux/engine/render/vulkan/transfer/Submission.hpp` | Address-stable `SubmissionQueue`、`CommandBatch`、真实 `SubmissionTicket`；不能与 Legacy 老 timeline 又建两份 GPU 完成权威 |

## 31.2 正确的端到端数据流程

```text
Worker / Asset Loader / Plugin producer
   ├── Persistent asset / immutable source bytes (shared owned payload or validated BlobRef)
   └── request ID / schema / byte count / generation
       ↓ bounded UPLOAD admission (independent from small CONTROL/PROGRAM)
RenderTransport UPLOAD Lane
       ↓ Server owner validates full packet and reserves Resource/Queue budget
Native Transfer Service [one physical GPU submission truth]
       ├── staging ring / validated mip/cube/row pitch / buffer offsets
       ├── typed Mesh / Texture2D / Cube / Region / Mip jobs
       ├── DEDICATED_TRANSFER → Submit via existing R4 SubmissionQueue
       └── QUEUE_ALIASED → Record-only / graphics-submit on sole queue owner
       ↓ actual producer SubmissionTicket + layout/queue-family state
Resource-ready acquisition / Native Graph import & acquire barriers
       ↓ GPU consumer waits on real transfer provenance (not 'Upload operation accepted')
Native Graph execution (MeshDraw/TextureSample/Material) → release / retirement
```

**原则**：`UPLOAD accepted` ≠ `native scheduled` ≠ `GPU ready` ≠ `consumer-completed`。这些状态必须分别可观察；渲染资源未 Ready 时遵循明确合法 fallback 或不调度需要它的 Pass，而不能读取尚未完成的 VkBuffer/Image。

## 31.3 大小不同的数据走不同交通机制

- **低频 CONTROL**：Create/Attach/Detach、Scene/Effect/Target 的身份事务，数据体积小但要求确认/Reply。
- **每帧 PROGRAM**：Camera/Grid/Light/instance 等经 Schema 的紧凑变更或必要的 small scalar blob；有版本和提交阶段，不能用大 Texture Blob 阻塞。
- **大体积 UPLOAD**：Mesh VBO/IBO、Texture Mips/Cube/Regions、Gaussian/PointCloud chunks、预生成的 Shader/Material asset payload；拥有源内存的生命周期、队列长度与**字节级**背压，允许异步传输提交。
- **Native GPU Transfer Queue**：设备/driver 决定是否独立 VkQueue；仅选择真实可用的 dedicated queue 才作并发执行，Queue alias 时同一队列由一个 Owner 串行提交。协议 UPLOAD Lane 无需因此总是开独立 GPU thread。

`PROGRAM` 不应被重复用于传大量单独 texture slice；反之可变化的少量材质参数也不应每帧发完整 Texture asset。上传接口不能只暴露裸源指针而不保留拥有者；ZERO-COPY 只在上游 buffer 生命周期/协议同一地址域合法时成立，不因“使用 span”就假定零拷贝。

## 31.4 Legacy GpuTransferPipeline 中必须保留的专业契约

- **队列别名退化**：Dedicated Queue 时 worker 可 staging/record/submit；Queue Handle 别名时不能让 transfer worker 同时 submit graphics queue，必须 record-only，由唯一 queue owner 最终提交。Shared Queue 不等于独立 Queue Family。
- **QFOT**：Transfer queue family 不同且资源 exclusive sharing 时，需要 release/acquire ownership transfer + 正确 semaphore wait/Stage/Layout；不可只做普通 pipeline barrier。不同 Queue 但同 Family 仍需跨队列执行/内存同步。
- **稳定的源 Payload**：Legacy MeshTransferTask `data_owner` pin VBO/IBO 原始数据，TextureMipRef `shared_ptr<const void>` 保持每 Mip 的 bytes；当前要求采用可追踪 immutable owner/BlobRef，不允许 packet 消费后悬垂原始指针。
- **精确字节计划**：Texture2DUploadPlan 的每 Mip offsets/total bytes、Cube face stride、region row_pitch/format/chunk limits 都在 Server 验证。Transfer worker 不得二次猜测行对齐或从一个已丢失的用户配置重算。
- **有界并发**：Legacy 线程仅接受明确的 Mesh/Texture/Cube 任务，不是 `submit(F&&)` 任意任务线程池；BatchSlotLease 直到 GPU/GraphicsAcquire 证明后才回收 Command Pool 与 Staging。
- **停止与失败**：所有已接受 Request 应有最终 Reply/Failure/Abandon；部分 Vulkan Submit 成功、后继 Submit 失败时，不能通过一个假定“全部完成”的最终 Ticket 提前释放任何 staging/mesh/image。

## 31.5 与 R4 的合并要求（禁止双执行真相）

保留 Legacy **两级有界队列、专用大资源任务、Staging/Record-only/Dedicated 模式和精确 QFOT 逻辑的价值**。但是在当前 R4 已有 `SubmissionQueue` / `SubmissionTicket` / `RetirementQueue` 的前提下，Legacy 内部旧 timeline serial/Queue owner/command-pool 控制不能原样并排复制。

建议先用 RenderBackend owner-thread + `pollUpload` + 当前 SubmissionQueue 实现一条**真实 Texture + Mesh** UPLOAD-to-GPU 纵向路径；保持从 Server 接受包到 GPU ready 的完整状态。如果可测数据表明独立 CPU staging/recording worker 确有收益，可加入一个**狭窄、仅服务 transfer 的 worker**，但仍向 R4 唯一 SubmissionQueue 提交真实作业或由唯一 queue owner 提交。它不拥有第二个 FrameLoop、Renderer、Layout State Authority 或独立的第二种“完成凭据”。

GPU enqueue 是 Native SubmissionQueue 的批处理边界，不是 C++ `std::async` 或意图队列被推入就算完成。Shader Graph 本来已有 Queue dependency/ResourceUse，Upload-produced resource 用统一 `readyTicket + NativeResourceState + family` 作为 Graph import；不要另建隐式 Transfer Shader dependency path。

## 31.6 测试与性能要求

| 反例/压力 | 必需断言 |
|---|---|
| 64 包/256MiB 默认预算不足时 | 精确 BACKPRESSURED，已准备 Payload/Reply 可恢复，不阻塞 CONTROL/PROGRAM |
| Mesh + Texture 同时上传 | 各自源内存长于 worker consumption，完成结果归正确 owner，不串 AssetId/Generation |
| Dedicated Transfer vs aliased Queue | 两种分支真实 GPU 正确；别名同 handle 没有并发 submit race |
| Transfer family != graphics family | release/acquire、Stage/Access、layout、timeline wait; SyncVal 0 error |
| Texture mip/region/cube | image extent/row pitch/format/layer offsets、chunk staging bytes oracle，无越界 |
| Reupload/Rebind/Delete-in-flight | old backing/tickets 保持到最后消费者，bindless slot 不提前复用 |
| Stop + producer late + partial submit | admission/replies/accepted native jobs drain，资源计数/validation 0 |
| 资源少变/不变 | 不重新传整份 VBO/Texture，不因一个小参数变更而触发 full upload |
| 可比性能 | 包括 CPU 主线程提交延迟、UPLOAD p50/p95、实际 copy bytes、staging 峰值、Graphics GPU stall、VRAM、高压 backpressure |

**阶段入口**：R3-B 先把 ResourcePlan 的 `Upload/Ready/Access` 语义与数据 Scope 固定；R3-C 接正统 Graph import/cross-queue; R3-D 第一条真实 Mesh/Texture UPLOAD + pool/retirement；R3-E Engine/Plugin assets DataProjection 走中立 Lane。任何工作单都不得把 UPLOAD 写成“已有传输能力实现完毕”。
