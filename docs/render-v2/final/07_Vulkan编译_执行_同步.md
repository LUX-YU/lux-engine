# 07 — Vulkan Graph Native Compiler、Descriptor、Barrier、队列、内存与录制

## 7.1 R4 是 native 基础，不是“Graph 已实现”

当前 R4（I `83ffbb6d9859d87ca62a8a2acbb0083c2a8de420`, V `7de3ddaa3f7614995745b41d73dfc98302310a4c`）已验收 `VulkanInstance/VulkanDevice/VulkanAllocator`, Buffer/Image、DescriptorSetLayout/Pool、ShaderModule/PipelineLayout/ComputePipeline、bounded staging、`SubmissionQueue`、`RetirementQueue`。其验证范围是 Windows/MSVC 单 GPU/单 queue native Foundation，**不是** Graphics pipeline catalog、多 Queue native dependency plan、完整 Graphics RenderGraph/Frame/Target。后续必须扩展现有 `render_vulkan` 组件的机制，不能新建平行 Vulkan Foundation。

## 7.2 VulkanGraphCompiler C9–C13 阶段

| 阶段 | 主要输入 | 必须输出 | Fail / fallback |
| --- | --- | --- | --- |
| C9 Layout/Shader | LogicalPlan + ShaderInterface/PassSchema + device caps | `LayoutPlan`，SPIR-V relocated validated shaders，Descriptor layouts/pipeline layout | 类型/shape/limit mismatch fail；last-good |
| C10 Physical resources | logical lifetimes/format/sample/size/history/alias eligibility | PhysicalResourcePlan，per-FIF allocations、compatibility class、import/export states | memory budget fail or explicit no-alias fallback |
| C11 Native pass execution | shader variant/graphics/compute/transfer pass contract | full PSO, render state, immutable pass binding/record recipes | missing shader/PSO fail at cold compile; never lazy record |
| C12 Queue/sync | logical hazards + stage/access/queue/device caps + external sync | BarrierProgram, queue family transfers, submit waits/signals, target final layout | unsupported queue→single-queue fallback only when semantically correct |
| C13 Executable assembly | C9–C12 complete candidates | `ExecutableGraphPlan`, all native owners/code pins/import slot recipes, validation/diagnostic tables | candidate rollback, old plan stays until in-flight safe retire |

`VulkanGraphCompiler` 可以是 namespace-scope compile function + private scratch/transaction object；不把它变成持有 Scene/Feature/Device/Target 的全局 Service。`ExecutableGraphPlan` 是实际运行所需一切稳定事实，生命周期明确，不能再在 `record()` 里查 Shader 名或缓存注册。

## 7.3 同步正确性合同（Synchronization2）

每个 GraphAccess 需能产生真实的 `VkPipelineStageFlags2`, `VkAccessFlags2`, image layout、aspect/mip/layer range 或 Buffer offset/range、source/destination queue family。完整 hazard 推导还需包括 implicit reads（attachment LOAD、Depth/Stencil load/store、present read、copy/resolve）和 shader multi-stage usages。对 unavailable/unsupported usage 标明 native compile fail；不把 native validation error 当 success。

- **同 queue**：在 producer→consumer 合适边界提交 full `vkCmdPipelineBarrier2` with non-empty overlapping execution scopes；只在语义允许时才 coalesce/barrier hoist。
- **跨 queue**：判断 `same queue family / different queue family`；使用正确的 wait/signal timeline/binary semaphore 顺序、queue family ownership release/acquire（需要时）和 image layout transition。只发 `NONE/NONE` 两端普通 pipeline barriers 不能被当作已经建成依赖链。
- **跨帧**：GPU resources/inflight descriptor/indirect/staging 带唯一 native submission evidence；不能以 `frame_serial - frames_in_flight` 算“已完成”。Read-after-write/write-after-read 应使用实际已完成 fence/serial。
- **Device Lost**：精确保留 `VkResult` 并将 Session 进入 terminal failure，阻断进一步提交；不伪造已经完成的 GPU serial，也不让析构依赖无法等待的 fence 永久挂住。

## 7.4 Multi-Queue 的真实边界与保守 fallback

R4 `SubmissionQueue` 仅持一个 native queue/family。Graph native compiler 若要用 `Graphics / Compute / Transfer` 多队列，需要扩展真实的 queue owner domain：明确每个 Queue 的 serial owner、跨 Queue ticket 完成谓词、Submit dependency/semaphore、CPU external serialization、原生资源 owner。**禁止复制三个独立 `SubmissionQueue` 就宣称 async compute 完成**。

`Pass` 声明 eligible queue，而不是硬指定无条件 Async。Compiler 可基于 device caps/queue topology/实际依赖决定 async eligibility 和 schedule；正确的单 queue fallback 始终存在。异步 Queue 可以变慢，应比较 GPU timestamp/perf，但不能仅以 enum 或 submitted queue_count 证明并行。

## 7.5 Transient memory aliasing 和物理资源表

`PhysicalResourcePlan` 标记：format/extent/sample/usage/heap flags/alignment/memory type、lifetime、import flag、alias compatibility group 和 **actual GPU overlap proof**。仅 overlapping lifetime 和 queue concurrency 排除后才复用 backing；aliasing 后续 write/transition 需要正确的 memory dependencies/undefined-content 初始化策略。Debug `no_alias` 与 optional initialized-poison mode 必须能运行同一 workload 进行 GPU readback 对照。

不要为 Graph transient 再建第二个 runtime-wide VMA allocator；借 R4 `VulkanAllocator`，由现有 native graph resource owner 管理已创建的 Buffer/Image lease 以及 in-flight retirement。资源池限容量且支持压力/峰值 telemetry；`alloc failure` 保留旧 plan/new candidate error，绝不 drop frame 并声称成功。

## 7.6 Descriptor 与 Shader 完整预编译

`LayoutPlan` 负责 shared/feature/pass local descriptors 的 full shape；Native compiler 创建所有 stable layouts、pipeline layouts、shader modules、PSO/variants、fixed binding recipes，并预备每帧的有界 transient descriptor pool/set slots、dynamic offsets/arrays。Graph `record()` 只根据 prebound indexes/explicit params 获取实际已验证的 native handles；不能每帧 `getOrCreatePipeline`, `vkCreateDescriptorSetLayout`, shaderc, SPIR-V patch, global map lookup。

R4 的 `writeStorageBuffer` 只是 Foundation smoke；后续必须补真实 sampled/storage images、sampler、uniform, descriptor indexing, dynamic offsets, arrays, update-after-bind/variable descriptor count capability-gated 语义，且通过实测 Shader 输出验证，不得用一个 storage descriptor 验收全部绑定系统。

## 7.7 Graphics / Attachment / local read

需要完整的 GraphicsPipeline owner（shader stages、vertex/mesh/point source、raster/depth/stencil/blend、topology、MSAA、dynamic rendering/render pass key）、multiple color/depth/stencil attachment、per-aspect load/store、clear/resolve、final target layout/present/export。

`LOAD` 视为读取旧 Attachment 内容并进入依赖分析；`CLEAR/DONT_CARE` 对 undefined/input correctness 必须明确。Multi-view output array-layer、Depth/Stencil separate aspects、local read/subpass input、tile-based GPU 的 dynamic rendering local read 可以作为 device capability variant 选择；不支持时走保守 separate pass/output path，带图像等价验收。Shader input attachment 与 sampled image 不应在 backend 伪装成语义相同的任意访问。

## 7.8 Vulkan Multiview（可选优化，不是另一套 Feature）

兼容 View group 才允许 `VkRenderingInfo::viewMask` + layered attachment + Shader per-View index。条件：`VkPhysicalDeviceMultiviewFeatures::multiview`，支持的 view count/layer count，compatible formats/sample/extent、Shader/pipeline state、DeviceCaps、per-view transforms/viewport/scissor strategy、per-view draw visibility/indirect payload 必须合法。

当 View A/B 的可见集合不同，Compiler 不能假定 `vkCmdDraw` 共用的所有实例都恰好属于两个 View；可选 per-view visibility bit/instance data/indirect work or fallback separate draws。GPU 输出 readback 与独立双 View 的正确图像容差一致，且必须实测 GPU 负担、不要为了减少 CPU drawcall 而显著增加 overdraw。

## 7.9 Graph Executor 与 native owner

```text
FrameExecutor (Backend thread)
   → resolves Scene work and compatible View group per-frame scratch
   → fills FrameGraphBindings (per frame / view / target)
   → ExecutableGraphPlan::record (precompiled pass recipes)
   → VulkanFrameDriver acquire/submit/readback/present
   → update GPU last-use tickets → RetirementQueue.collect(completed)
```

`ExecutableGraphPlan` owns immutable native PSO/layout/barrier programs + Feature code pin; `GraphFrameState` owns per-FIF scratch/decriptor pools/command streams; physical backing owner tied to plan/scene/view as appropriate. `PassRecordContext` 是受限借用，不能直接暴露 `VulkanBackend` mutable root。第三方 Feature 使用 public typed `GraphicsPassContext`/`ComputePassContext` 等限定 GPU 指令接口，内部编译成稳定函数表/record recipe；仅极明确、经批准的 Vulkan extension tier 允许有限原生能力。

资源图拓扑/Target resize/Shader variant/Feature unload 的替换执行 `prepare new complete plan → validate → no-fail-ish publish → fence-retire old plan/resources/callback pin`；失败不破坏旧合法状态。同资源的旧 descriptor 与提交仍有效直到最后 native reference completed。不可将已退休 plan 的裸 spans 存到下一帧。

## 7.10 实际 GPU 验收

- Subresource image mip chain, Buffer overlapping/non-overlapping offsets; barrier/image range 精确；Validation **0 errors**。
- Cross-queue compute→graphics→transfer readback，故意反序/同时写、queue ownership 家族差异、单 queue fallback；读回值完整一致。
- Async alias on/off、noalias poison/guard mode、2/3 FIF、resize/rebuild/stop/Device Lost injection、GPU fence retirement 前不 destroy。
- 真实 graphics PSO + depth color/MSAA blend/stencil/resolve、multi render targets 和 present/offscreen、local read fallback。
- 真实 Shader layout placement/merged shared shapes/descriptor budget，native compilation last-good rollback。
- 1 Graph / N View Plan reuse + independent Camera outputs + Multiview readback compare，P50/P95 record CPU、GPU timestamps、allocation、compile_count。

**以 59/59 R4 CTest 为回归基线。** 对新 Graph/native 增量另加 GPU/sanitizer/prod build 正例，不能通过 fake native device 代替真实验证。
