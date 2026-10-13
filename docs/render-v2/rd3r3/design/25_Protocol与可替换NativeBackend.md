# 25 — Engine ↔ Server 仅通过中立协议；Native Provider 可替换

## 25.1 职责断面

应用 Native bootstrap 选择 Vulkan 或未来 D3D12 Provider，并在运行前选择 GPU；Engine/Editor/ECS/Plugin 只拿 `RenderRuntime` neutral client。中立 `RenderBackend` 拥有 Scene/View/Effect/Target/Attachment 和唯一 FrameLoop，通过一个必要的 Native Session Port 调用具体图形 API。当前 `runtime/Backend.hpp` 直接接受 `vulkan::BackendOptions/EffectSource` 是 P2-A 的实际过渡而非最终公开 API。绝不要求 Engine `runtime.vulkanDevice()` 来编写 RenderSystem。

```text
Platform/App Native Factory  [Vk instance/physical device OR D3D12 adapter]
        │                      \ one concrete native implementation chosen
        ▼                       
RenderBackend (neutral owner-thread)  ←PROGRAM CONTROL UPLOAD→ RenderRuntime Client
        │  neutral Effect/ResourcePlan/GraphIR, data descriptors and target requirements
        ▼
Native session port (single narrow backend seam; no parallel frame loop)
        ├── Vulkan: bootstrap InstanceContext + one DeviceSession (Device/ResourceContext)
        └── Future D3D12: own Device/DescriptorHeap/Barrier/CommandQueue/Swapchain
```

`NativeSessionPort` 可以作为一个局部固定 Ops 表/小的 type-erased session 值，与 Native Provider 对象一道在 bootstrap 装配；**不是**暴露给 RenderFeature 和 Engine 的巨大 `IRendererService` 虚接口。哪几项操作成为 Ops 必须由真实 Compile/Prepare/Execute/ResourceFactory/Target/Retire 消费者证明。单 Native Provider 只能有一个最终 GPU work scheduler，不能创建第二 FrameLoop/Backpressure system。

## 25.2 继续沿用 R2

PROGRAM、CONTROL、UPLOAD 三 Lane、Reply Arena、Admission、Stop/Drain、Diagnostic 端口和 Protocol codegen 都已经真实存在。新 DataProjection/Effect/Attachment 消息应为同样的版本化 wire 语义，在 Handler 检查 owner/generation/data schema，并在完整接受后才 commit；不允许 Runtime 内并行维护第四个 EventBus。场景/效果变更的 hot revision 与 GPU accepted tickets 必须区分。

## 25.3 Backend 可移植性的边界

平台中立的 Compiled ResourcePlan/GraphIR/ShaderSemantics 可复用，具体 Vulkan 的 SPIR-V、DescriptorSet/VkPipeline/Sync2、WSI 对应到 D3D12 的 DXIL/RootSignature/PSO/ResourceState/Queues/DXGI；这是独立 Native Compiler/Execution 的工作。数据 Projection 不知道这些，因此更换 Backend 不修改 Engine Grid/Mesh/Light Component 转换代码。允许 provider 明确声明不支持某 Feature/Native command；必须走合法 fallback 或配置错误，不造假的等效性。

## 25.4 多设备不属于本轮

每 RenderBackend 恰好一个 DeviceSession，初始化选一次 GPU；`InstanceContext` 可以枚举多个 Adapter，却不等于当前运行多个 session。Future cross-GPU copies/device groups/async compute coexecution 单独设计，有真实消费者再增加；本期不出现 DeviceSessionId/sessions registry/跨 GPU Target Composition。


## 25.5 Bulk UPLOAD 是独立协议 Lane，不是裸 Vulkan worker API

当前 `RenderTransportCapacity` 已分别约束 `pending_uploads`、`upload_bytes`。Native Provider 的 `UploadConsumer/TransferService` 通过 Host/Server 内部接口接受已验证的 owned payload，产出 buffer/image 后续合法使用所需的 `ReadyTicket/NativeResourceState/QueueFamily`，并在资源的 LastUse 安全时退休；R2 client 不暴露 VkQueue/VMA。专用 Mesh/Texture/Chunk 大上传不能复用 PROGRAM 控制包到处传 `span`，亦不能在 D3D12 增加时改 Engine ECS Projection 的语义。详见第 31 章；P2-A Backend 的 Upload Lane 尚无 active consumer，状态为 NOT_IMPLEMENTED。
