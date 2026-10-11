# 09 — Vulkan Foundation、Native Graph、GPU 资源/同步/Queue/Recorder

## 9.1 保留已经正确的实现

R4 `VulkanInstance/Device/Allocator`、Buffer/Image/Sampler/ImageView、Staging/SubmissionQueue/RetirementQueue 等 move-only native RAII 保留；F3 已有的 LayoutPlan/PSO/Descriptor owner 保留；F4 `compileVulkanGraph` 是一个冷态自由算法、`ExecutableGraphPlan` 是复合 owner，属于正确方向。不要因 RD2 构建 `GpuResourceManager`、`VulkanFoundation2`、第二 RenderGraph 或第二 VMA Owner。

## 9.2 物理资源类别边界

- Buffer/Image/Sampler 当前是已实现的 Native primitive；Typed DataSet 使用它们作为 backing，不在 Renderer native enum 内注册每一种业务 `T`。
- DataSet 的 GPU backing 使用 `VulkanAllocator` 申请实际 Buffer/Image/Arena/Chunk；StoragePlan 指定 usage, access, alignment, capacity, shape 与 native descriptors。维持单个 Native owner + borrow/leases。
- Graph transient Buffer/Image 单独由 Native Plan/Frame Slot 负责；不能拿它当跨 Frame DataSet 的隐式 owner。
- 新的 Native primitive（如加速结构、Sparse Image、Video/RT）需要在 Vulkan-native 类型、同步/Shader 角色、DeviceCaps 上显式扩展；这是合法的局部更改，不意味着新的 Feature 基类。

## 9.3 CPU/Native Slot 与 GPU 真正完成

GPU 入场受实际 Queue Capacity 和 Submission Tickets 约束；native submit 可跨真实 graphics/compute/transfer queues + Sync2 family ownership。资源合法性检查包括 Imported backing shape/usage, VkDevice, memory overlap, current layout/stage/access, Queue owner/ready, valid descriptor, exact subrange 与身份版本。GPU 多 Queue Fallback 必须确实在单 Queue 上正确执行，不可以“只记录一个枚举”冒充。

**不同 VkImage/VkBuffer native handle 可能别名同一 VkDeviceMemory 及部分 range**，因此 Imported Alias 不可只按 handle 身份判定；根据内存绑定区间做 conservative overlapping check，若无显式别名关系则拒绝。句柄比较只是第一层。别名优化必须证明实际 queue happens-before、正确布局和访问掩码。

**Host readback**：Compute shader Storage Write / Transfer Write → Host Read 需要正确 stage/access dependency、真实 GPU completion 和必要 non-coherent invalidate。不能只认 `TRANSFER_WRITE`，也不能以 fence 完成替代 shader→host memory barrier。

## 9.4 Slot lease 和跨 View 调用

`FrameCapacity=3` 不意味着 1 个 Native Plan 只能同时服务 3 个 View；RD2/FINAL F5 目标是 immutable `NativePlan` + N bounded dynamic `InvocationSlots`（每次提交有独立 Command scratch, attachment views, descriptors/readonly snapshots, resource pins）。FrameSlot、ViewID、InvocationLeaseID 各自是不同身份，不能用 `ViewID % FIF` 猜测是否可复用。Slot 在所有参与 Queue 的 tickets 和外部 last use 完成后才可重新借用。静态 PSO/Descriptor layout/compile recipes 仍然共享，不能为每 View 克隆全套 Shader。

原 F4 当前 `ExecutableGraphPlan` 同时拥有 immutable recipes 与 per-FIF mutable slot，F5 才完成上述隔离；F4 仅需保证不堵死演进路径并关闭既有安全门禁。

## 9.5 Typed GPU operation 与 Hot Path

当前 F4 仅实际实现直接 procedural draw、dispatch、copy/readback，3D/cube/relative extent 和 vertex-stream graphics NOT_IMPLEMENTED。F8 的 GPU-driven Mesh/PointCloud 需要 Index/Vertex/Indirect/IndirectCount（受实际 device caps）并最终由一个 Native Recorder 处理。公开 `GraphicsPassContext`/`ComputePassContext`/`TransferPassContext` 是**受限的命令录制借用接口**，不能公开原生 VkDevice/Allocator 让插件绕过 Graph；也不能为自定义算法实现第二套 Native Scheduler。

编译后 hot path：`Frame bindings + precompiled recipe + bounded Slot → record+submit`，首方 C++ heap 0、无 lazy PSO/layout/Shader compile/name map、无 runtime TypeId 查找。跨 Scene/View 的 Scene shared GPU work 需要依赖证明，不自动重复运行完整 Scene graph。

## 9.6 Resize/替换/失败回滚

Native Storage 发生扩容、GPU ABI/Layout 修订、Image dimension change、Shader hot-reload：在 cold path 完整创建候选 Backing/Pipeline/Layout/Graph，校验所有 limit & authoring contracts，成功后安全 publish，旧对象按最后使用在后台由已有 Retire 队列**被动回收**。无需自建 worker；正常帧不要 `vkDeviceWaitIdle`。失败保留 old valid plan/visible snapshot，若目标已不兼容则显式错误/暂停目标而非绑定不正确旧数据。

## 9.7 F4 已知现实与不得误宣称

截至审查基线 `b9f9a909bc34`: Normal 146/146、Full ASan 146/146、真实 GPU Validation/SyncValidation 0 errors（测试路径），但 F4 **PARTIAL**：fixed binding comparable p50 相对 F3 +5.0420%，配对置信区间横跨 5%，仍未关闭；3D/cube/relative、vertex-stream 仍明确 NOT_IMPLEMENTED。早前源码审查发现应增加**负例/真实 GPU 测试以证实或排除**：直接 Compute→Host Readback barrier、imported alias physical overlap、导入后旧 ImageView/backing 的生命周期。不能把这些未经复现的静态风险描述为已经在实机触发。新 Native 设计不能修改已验收的 F2 逻辑语义来绕开它们。
