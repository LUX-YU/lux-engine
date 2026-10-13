# R3-E — Engine DataProjection、Native Provider 隔离、插件 SDK

**状态：未授权。** 输入 03/06/08/11/14/15/17/24/25/28。此阶段把曾经在 Legacy `RenderSyncStage/RenderFeatureSceneBinding` 的“ECS 观测、背压、插件集成”转换成新的中立可安装作者方案。

- Engine RenderSystem 保留 ECS O(changed) observer、entity leave、`prepare/commit/discard` backpressure 逻辑，但不要为每个 Feature 新造一个虚继承 `Proxy`。插件注册 `DataProjection` 表及 neutral DataSchema/Writer、变化观察、CodePin；RenderRuntime 接受 typed PROGRAM/UPLOAD 与 Schema versions，Backend Native 生成 GPU 更新。
- 以 `GridComponent` 或 Editor Grid Settings → GridData → native GridFeature shader output 和 `Mesh3D+Transform` → Scene instance resource delta 两条真实路径验证；插件新增自有 Gaussian/Custom Feature/Data/Projection 而无需 RenderSystem core switch。
- Native bootstrap 包含 InstanceContext；Backend 中立对象通过单 narrow NativeSession port 调 Vulkan DeviceSession；Engine/Editor/Plugin public include 与 installed consumer 禁止 Vulkan private header，非 native provider 不返回 VkDevice；不预先实现 D3D12。
- CodePin 覆盖 Plugin Feature Ops、Projection callbacks、已编译 native code/资源析构直至实际 GPU/CPU completion。R2 原 3 lanes/reply/admission/diagnostics/stop 不重写。
- 测 Project/Scene switch、Backpressure accepted/retry、ECS Component create/update/remove 与 View attach 时初始快照、Plugin unload/in-flight/native final, installed SDK independent C++20 compile, API closure。

不在此阶段把新 DataProjection 扩展到所有 36 项 Legacy Feature；后续 L1–L3 逐项真实迁移。I/V/STOP。


## Material Asset/插件 Node 与 Native UPLOAD 的边界

新增 MaterialSource/MaterialAsset 作为 Engine/Editor/Plugin 端独立数据源：Projection 或 Asset 系统只把版本化 MaterialAssetId、参数改变、贴图引用和（在需要时）owned cooked payload 经中立 PROGRAM/UPLOAD 发到后端；RenderSystem 不需要了解 MaterialNodeCatalog/GLSL/Vulkan。Material Node 类型注册与 RenderFeature 注册两种插件扩展各保留 CodeLease/CodePin，缺失类型或 ABI 明确拒绝，卸载时不能留下指向动态库代码的裸回调。UPLOAD 的 byte budget/stop/replies/late producer 必须验证，不要复制 Legacy RenderUploadClient + R2 RenderTransport 为两个永久独立通讯系统。
