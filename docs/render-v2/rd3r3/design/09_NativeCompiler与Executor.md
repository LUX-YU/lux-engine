# 09 — Native Compiler、Context、Executor 的职责边界

## 9.1 只有一个执行流水线

`RenderBackend` 拥有合法目标/Effect attachments 与一个 FrameLoop；Native Compiler 消费 GraphIR/ResourcePlan + selected Native Session + Target signature，产生唯一 DeviceSession 中的不可变 `vulkan::Graph`。`GraphExecutionContext` 持 per-attachment/Target variant 的 transient、descriptor instances、FIF slots、history import states、GPU accepted tickets；`Executor` 是录制/提交的算法，不能拥有另一份 Scene/View/Effect 权威。

```text
Neutral Effect / GraphIR + ResourcePlan + Target output declaration
 → Backend Native Port (one implementation selected at bootstrap)
 → Vulkan C9 Shader/Layout, C10 Resources, C11 Recipes, C12 Queue/Sync, C13 Assembly
 → immutable Graph code (per selected DeviceSession)
 → attach: PreparedBindings & RAII resource leases
 → Executor executes native passes in a valid order
 → final Target output / Present / explicit Readback
```

## 9.2 访问最窄的 Context

`InstanceContext` 归 Native bootstrap；仅用于 Instance/Validation/Adapter enumeration；`DeviceContext` 含 VkDevice/VMA/Queue/enabled caps；`ResourceContext` 提供需要的 native pools/staging/transfer service。创建 `MeshStorage` 仅要 ResourceContext 时不应依赖整个 RenderBackend；编译逻辑 IR 不应看到 Vulkan Context。DeviceSession 是设备会话持有者，不成为 UniversalManager；必要 Native Service 只在真实消费者出现时添加。

## 9.3 当前 P2-A 事实与待整改

P2-A 已把 Native owner 移进 `DeviceSession.hpp/.cpp`，但仍让 `InstanceContext` 成员在 Session 内，`DeviceSession` 位于 `render/runtime` 并由公共 assembly 接缝暴露 Vulkan 类型，旧 `ExecutionJob` 仍留在 `Objects.hpp`，这**不是最新目标模型**。后续只迁相关 Owner，不重写已确认正确的 R4 RAII、C0–C13、Sync2/Submit/LastUse、Queue/address stable 算法；先做小型真实 native consumer，再删除旧重复入口。

## 9.4 资源和同步

Native Storage 实际 backing 与 transient backing 分别由 ScopeResources 或 GraphExecutionContext 唯一拥有；`PreparedBindings` 是已保证寿命的直接引用/descriptor，不能再次声明 Buffer Owner。资源 Ready/LastUse 与 Queue Ticket 是 Native owner 的真实事实；CPU Lease 释放不可让已提交的 VkBuffer 立即析构。任意 Partial Submit Failure 无法构造完整 Join Ticket 时，保留所有已接受 work 到安全 teardown，而不是虚构成功凭据。

## 9.5 执行效率

资源 ID/Type/Key 到 backing 的解析发生在 attach/rebind；CommandBuffer Record 跳过多余 Pipeline/Descriptor bind，不能跳过真实 Alias/Queue hazard。Native `Graph` 跨多 View 使用同一 PSO/Shader，不复制代码；slot 数按实际附加并发增长，在冷态分配、热态固定分配量为零；同一 GraphId 可在非相邻的合法 Target 工作段执行，不做会破坏 Target 依赖的全局排序。

## 9.6 Native Provider / D3D12

Client、Engine/Editor、neutral backend、Feature authorship/GraphIR 不包含 `vulkan/vulkan.h` 或 DirectX headers。Native Provider 只需要在启动/编译/资源/执行/目标边界提供真实具体实现，不创建第二个 RenderBackend、第二个调度器或 Native Feature Proxy。D3D12 对相同中立接口独立实现 Device/PSO/DXIL/RootSignature/Resources/Barrier/Swapchain，与 Vulkan Shader Output 不混用。详细边界见第 25 章。


## 9.7 已有 Native 快路机制与真实 UPLOAD Ready

Legacy Native Compiler 还包括: Dynamic local-read merged rendering scopes、Load/Store/Conditional 证明、reused bind recipes、Kernel compiled execution program、cross-queue submit templates、VMA lazy/transient pool 与完整 Resize Candidate，必须按第 29/32 章纳入 R3-C 的真实验收。当前 R4 C0–C13 已具备的部分优先复用，禁止把 Legacy 的错误 split-barrier/全局固定 DS 号恢复。

Mesh/Texture 的 CPU Payload 通过 R2 UPLOAD（单独的 byte budget）进入 Native Transfer Service。Dedicated Transfer Queue 与 aliased Graphics Queue 使用不同提交路线，但均由唯一 R4 SubmissionQueue/Ticket 给出 GPU ready 事实。PreparedBindings 只有在真实 readyTicket、Queue family、layout 与 memory barrier 完整证明后，才能供 Graph 使用；不能因为 Upload Reply 已提交就提前声明资源可读。精确方案第 31 章。
