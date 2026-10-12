# RD3-P5 — Effect 热更事务、Installed SDK/Plugin 与 Engine/Editor 闭环（DRAFT，NOT_AUTHORIZED）

## 目标

在真实 Client/Server、Effect/Graph、Native Executor、ResourceDomain 四条路径已正确的基础上，补齐 RenderEffect 的共享编辑语义、Dynamic Plugin ABI/CodePin、Editor RenderSystem/Assets/Target 端到端消费者，而非增加一组新的管理类。

## 必做

1. `UpdateEffect` 以 expected revision 完整构建 candidate，收集所有引用此 Effect 的 View + 相容 Target/Device Signature，先验证完整 Native Graph、resources/capacity/plugin lease，再在安全帧点整体 Publish；失败 LastGood，不存在半数 View 仍旧的“成功”。
2. Hot params 仅刷新 Effect 数据快照和 Native bindings，Scene Light/Transform 更新保持各自 Domain 权威，不因 Camera/Exposure 数值热更新编译 Shader。
3. 独立外部 installed SDK C++20/C ABI plugins：consume built-in LightGpu、declare new GaussianChunkGpu 与第二插件 consumer；ABI mismatch、Type ID collision、DLL unload after GPU in-flight and CodePin、Host metadata deep copy。
4. Engine/Editor `RenderSystem` 只使用 RenderRuntime Client 协议，不 include Vulkan private headers；ViewTarget 与 Editor UI/Canvas/Gizmo/Picking 通过完整 Graph 声明 output，不走第二 UI Renderer。无 ECS 的工具/2D 也能直接发布 GPU 数据。
5. 双窗口、Offscreen/Surface、Editor 多 View、停止/重开 Project、Device Lost terminal、资源/Scene remove/recreate 的真实 GPU/ASan tests。

## 门禁

第三方独立编译/运行、GPU pixel/readback、Feature config edits atomic A/B、Upload backpressure O(D)、Native memory peak、same Graph shared across Scenes、多 View/Target 正确 final output、Public include closure、原 Legacy/历史性能基线、I/V/STOP。任何只跑内仓库 fixture 的插件都只能 PARTIAL。用户另行批准产品 Full Cutover 阶段。
