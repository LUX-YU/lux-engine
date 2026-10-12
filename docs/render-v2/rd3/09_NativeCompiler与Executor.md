# 09 — Backend Native Graph、Executor 与 GraphExecutionContext

## 9.1 C9–C13 之后的 Native Graph 是什么

`vulkan::Graph` 为不可变、无 `execute()` 方法的完整 Native 程序，含 GPU pass/PSO/Layout、当前 Device 句柄的 RAII owner lease、资源规划、Image/Buffer use/alias、Barrier/Queue/Submission 和预编 Binding Recipes。它是“数据”而不必 `trivially_copyable`：Pipeline 需要真实 Device lifetime，Graph 缓存的共享只共享不可变 Native owner，不共享 per-view Descriptor/Transient/FIF scratch。

```cpp
// 目标语义
namespace lux::render::vulkan {
  struct Graph { /* immutable compiled code + pinned native owners */ };
  class GraphExecutionContext { /* mutable resources, slots, states, tickets */ };
  class Executor {
  public:
    RenderResult<GraphSubmission> execute(
      const Graph&, const View&, Target&, GraphExecutionContext&) noexcept;
  };
}
```

此处 `View`/`Target` 仍无 Vulkan 头，具体 backend TargetImage/Acquire/Present 由 Executor/Backend Native 资源表解析。对多 View target composite，Context 与 Graph 声明明确的多 View 输入，允许 `execute(graph,span<views>,target,ctx)` 的已验证调用，不隐式依序执行多个独立后处理图。

## 9.2 GraphExecutionContext 的必要成员

- 对应 `GraphId` 与具体 View(s)/Target compatibility 的稳定执行关联、generation；Effect 更新时旧新 Context 可短暂并存。
- 实际已校验的 ResourceDomain Resolve/Native resource ref、Descriptor sets、DynamicOffsets、ImageViews、Sampler、native state ranges 等。
- 有界数量的 FIF ExecutionSlots：per-pass scratch、transient Buffer/Image、accepted values、pass/cell/queue tickets、当前正确 `VkImageLayout`/Access/Queue family。
- Scene Pass legal donor 的真实同帧 submission/last-use lease；对同一 Device 的别名/Imported GPU memory overlaps 必须在执行前拒绝未经证明的危险情况。
- 最后的 GPU Submission 结果和 output ready/retirement 信息。History 语义归 View Domain；Context 仅持执行所需 image/version lease。

**这个 Context 的创建容量以实际活跃 View/并发执行需求为界**，不能按 `max_views × FIF` 给每个 Graph variant 预建满额所有 GPU attachment；正常 Record 不热分配，也不能在 GPU 仍读旧 slot 时复用。

## 9.3 Executor 的唯一执行流程

```text
backend frame begin → acquire Target native images and attach final output contract
preflight all actual jobs: capacity/slots/import readiness/queue submission budget
for each prepared GraphId group respecting final target job order:
  for job: resolve pre-bound Context → validate actual Native imports/tickets
           → optional proven Scene-pass reuse
           → bind/record native pass recipes (minimize redundant cmd binding)
           → Sync2+Queue submit → GraphSubmission receipt / output states
finalize declared final Target image → Present or Offscreen ready/export
retire old Graph/Context/Asset after true Queue/WSI completion
```

`execute()` 负责整个 Graph 的 Pass，包括后处理和必要合成，**不会**在图外再发隐藏图形渲染工作。GPU 提交成功不代表像素已经可 CPU 读取；readback 必须等 GraphSubmission 完成并使用正确 Host publication barrier。允许多 Queue 真 Sync2 + single queue fallback，所有 Queue 失败/partial submit 不制造虚假 join。

## 9.4 稳定 Native 状态的录制成本

Compiler 固定 Pipeline/Descriptor/Resource/Barrier recipes；Executor 只对当前 CommandBuffer 保持本地 binding cache，尽可能少 `vkCmdBindPipeline`, `vkCmdBindDescriptorSets`, `vkCmdPipelineBarrier2`，不跨 CommandBuffer 误用状态。C++ first-party heap 在固定 Graph/Context capacity 热态为零。计量需区分 native driver allocation bytes（可能不可观测）与 C++ heap allocation count；一个微基准 PASS 不代表大型 Frame PASS。

## 9.5 WSI、Offscreen、ExternalInterop

`Target` 逻辑对象独立；Vulkan backend/target 管理真实 Surface/Swapchain Image、Acquire/Present semaphores/fences，Offscreen backing/ImageViews/Readback pool。双窗口下 Present completion 不等于 graphics Queue completion，重建/最小化时不能复用 pending binary semaphore。Resize 先创建新候选与必要 Context/Graph target variant，失败继续 last-good；old native resources 经 Queue/Present 双证据 safe retire。

外部 CUDA/Vulkan 等零拷贝只经显式 Backend native interop contract、external waits/signals/UUID/memory types，不能把 `VkBuffer` 作为 Editor Feature 公共成员。其他 Backend 可拒绝未支持互操作，不能声称全平台同功能。

## 9.6 生命周期与故障

Native Graph、Pipeline、Layout owner 的生命周期不得短于任何 Context 或在途 command buffer；Context 销毁不得短于其全部 Queue Ticket；Imported GPU Asset 的 CPU release 只撤销新引用，不能释放在途 physical Buffer/Image；Partial Submit 或 Device Lost 通过明确 Terminal/Retirement path 处理。`waitIdle` 只能在确需 session teardown/rebuild 时合法使用，不是每个 hot update 的同步补丁。

## 9.7 现状与迁移事实

当前 `ExecutableGraphPlan` 共享 immutable `NativeGraphCode`，`NativeGraphInvocations` 持 move-only `NativeGraphBacking/GraphSlot`，真实 Vulkan Sync2/Queue/ABI 正负例已经实现。新设计需要**合并职责和清理命名/双份状态**，不是在旧接口上再套 `Graph+GraphExecutionContext+Executor` 三层空转发。由于 F4/F5/F6.b 都有历史 PARTIAL 性能资格，任何移植影响必须提交同负载比较、I/V 完整原始证据。
