# 21 — Vulkan 三级 Context 与单 DeviceSession 的正确父子关系

## 21.1 Legacy 的价值与必要修订

Legacy `VulkanContext.hpp` 明确分 `InstanceContext`（VkInstance/debug/扩展）、`DeviceContext`（physical/logical device、VMA、Queue/Caps）、`ResourceContext`（Descriptor/Command pools）；Legacy `RenderContext` 才是过于庞大的渲染全局服务中心。新架构保留**能力分层**，不保留巨型 `RenderContext`、或认为 Context 就是新的 Renderer。

```text
Vulkan Native Bootstrap  [owns lifetime]
  └── InstanceContext (VkInstance, Debug/Validation, adapter discovery)
         └── borrows by one DeviceSession
              ├── DeviceContext (selected VkDevice, VMA, stable SubmissionQueue, enabled caps)
              ├── ResourceContext (descriptor/staging/transfer resource services)
              ├── Session-scoped Persistent Storage / Native Graph programs
              ├── ExecutionContexts / Native targets
              └── GPU ticket-based retirement
```

**InstanceContext 的 owner 在 DeviceSession 外**，且其稳定地址/析构必须在 Session 之后。当前 P2-A `DeviceSession::instance` 是真实已实装但不满足新目标的过渡结构。Native bootstrap 可把 `unique_ptr<InstanceContext>` 与借用它的 `unique_ptr<DeviceSession>` 封装进一个小的 move-only Native Owner Bundle，保证声明/销毁顺序；此 Bundle 是独占装配值，不需要 ContextManager 或 Sessions SlotMap。

## 21.2 多 GPU 的边界

启动先从 InstanceContext 枚举所有适配器，按用户 device policy/UUID/能力/Present support 选择一次；本 RenderBackend 一次只建一个 DeviceSession，不新增 DeviceSessionId，不实现跨 VkDevice Buffer/WSI 隐式共享。底层 InstanceContext/DeviceContext 不使用静态全局唯一 VkDevice，这让将来独立 GPU compute session 成为可能，但这不是当前可交付功能。

## 21.3 只暴露必要能力

InstanceContext 只负责 physical/device discovery/instance caps；DeviceContext 负责 logical device/queues/VMA/Queue ownership，正确处理同一 VkQueue 对应多个角色只实际拥有一个 SubmissionQueue；ResourceContext 提供 Descriptor、Staging/Transfer/Upload/Retirement 的真服务，但不默认直接持所有 Mesh/Texture/Light 业务资源，也不再额外创建与当前 GraphExecutionContext/SubmissionQueue 重复的 CommandPool。

专业 Storage Factory 接受所需最窄 Context：纯 Vulkan Buffer 使用 ResourceContext，ShaderPSO 使用 DeviceContext+Shader ABI，纯 Graph Author 无 Device Context。Native Factory/Session 只能由平台 Vulkan 实现建立，Engine/ECS 不持 Native 指针。

## 21.4 关闭与重建

停止新请求/提交 → Backend worker join/drain → attach use leases release → Native contexts 等真实 queue tickets → ScopedResources/Native Targets/compiled Shader/PSO 释放 → ResourceContext → queues/VMA/VkDevice → Native bootstrap InstanceContext。Device Lost 状态下不能用旧 Vk handles 重建新 Session；需要应用层受控重建 Backend/DeviceBundle，不宣称自动恢复 GPU。

## 21.5 设备选择的可移植呈现

neutral `AdapterSelection` 描述用户设备选择的政策/能力目标，不包含 VkPhysicalDevice；Vulkan Provider 将其解析到当前 Instance 的 physical device。Surface present 必须检查该 device 对目标 surface queue family 的支持，Offscreen 可以选择无 present 的 GPU。D3D12 Device Factory 可以用同一中立意图选择对应 Adapter，实现不同但 Engine ECS 源码不应改动。
