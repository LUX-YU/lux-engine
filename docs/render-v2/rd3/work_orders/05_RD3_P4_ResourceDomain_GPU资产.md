# RD3-P4 — 层级 ResourceDomain、资产句柄与 ECS-owned CPU Authority（DRAFT，NOT_AUTHORIZED）

## 核心边界

唯一公共 `ResourceDomain + ResourceRegistry` 在 Backend 管理；Root/Scene/View 资源归属且子域向祖先 read-only 查找。本域编辑、Type+Key 歧义/遮蔽规则、生成式 Domain/Asset handles、跨 Device 拒绝是冷态 correctness。Root Mesh/Texture 等共享 backing，Scene Light/Instance 独立，View Camera/History 独立。不能每 View/Feature 各建一份 VulkanResourceRegistry。

## 实现优先级

1. 从现有 R4 Buffer/Image/Arena/Queue/Retirement 接一个**真实 GPU Resource** 的 retain/release/last-use/backing generation 路径；CPU release 决定业务引用合法性，Native completion 决定物理释放。缓存热度/TTL/LRU 为未来策略接口，不在本阶段实现大型驱逐框架。
2. Engine ECS/Asset 仍是 CPU authoring authority。RenderSystem 持 SceneId、resource handle、generation/dirty cursor、pending accepted deltas，不在 Backend 重建 CPU Light/Transform ECS 镜像。显示真实 O(D) dirty projection、backpressure 后不丢 CREATE/DELETE/generation。
3. GPU-only Particle/HZB History 借 Graph 真实写入与 Ready 路径；没有 `.get<Particle>()` 伪读值 API。
4. 多 Type/Key `Light.main`/`Light.preview` 和 MaterialBuffer 一 Shader 真 GPU output；当前 PassSchema/BindingLayout 保持完整实际 owner 验证。
5. Domain destroy/reparent（如支持）必须在逻辑撤销和旧 GPU in-flight/Plugin codepin 满足之前不释放 native owners。

## Gate

N=10k–1M、D=0/0.1/1/10/100%、random/clustered、本机 VRAM/Upload Queue memory、2 Scene shared Mesh、View History resize、two-owner Shader/ASan/SyncValidation/C++20 installed consumer 和 source closure。不要引入 Universal TypedStorage/ECS Entity/BDA/SoA 统一框架以填满未来需求。每阶段独立 I/V/STOP。
