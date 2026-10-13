# R3-C — 真实 Target Final Graph、Native Executor/History/WSI

**状态：未授权，依赖 R3-B 验收。** 输入 07/09/10/13/17/26/27/28。首要目标恢复完整目标输出合同，不能为了保留 P1 offscreen readback 限定而放弃 Legacy 多 View/Target/Presentation。

- Target T 混合两个不同 RenderEffect，Compiler 将二者子图 namespaced 合成一份**最终 Target Graph**，明确 Layer/Rect/Blend/Resolve/Post/Output，且 Target 不保存第二份可编辑 Effect list；同 E 可供多个 Target，不错误重复 GPU 中间 producer。
- WSI/Offscreen Native Target 从真实 DeviceSession 内创建，处理 acquire/present/resize/minimize/restore/readback/fence；Present completion 与 Queue submission completion 不混淆，WSI GPU 工作不能藏在第二 Renderer。
- History per View 或 Attachment-private，按真实 Effect+Target+scale/format+camera cut epoch 失效与 pingpong fallback；多个 Scene/View Target 互不串扰。
- native Compile C9–C13 固定阶段与 Executor 可利用 Prepared Bindings、资源 alias、Native binding redundancy 优化；多 Queue Sync2、buffer byte/image mip/layer exact hazards、local read/conditional fallback/ready provenance 不降低已有 R4 资格。
- 测同 Target E1+E2 mixed pixel oracle、两 Target 一 View 不同 sizes、2/3 FIF、Window+Offscreen、GPU Validation/SyncVal 0 error、Partial submit/Stop/Device Lost 安全。未实现真实高级算法不构成此阶段功能。

只迁受影响的 Native Context/Target代码，不复制旧 Renderer FrameLoop、PresentationManager 或对原 SubmitQueue 进行完整重写；完成 I/V Push STOP。


## Legacy Compiler 深化验收（新增）

以第 29 章 Compiler 机制清单检验：Legacy mixed render scope / `inputRead`/LocalReadBoundary、first-writer CLEAR / last-use STORE_DONT_CARE、ConditionChain/fallback、first/subsequent View barrier、precompiled bind masks（尤其 Layout 改变需 rebind）、跨队列 Submit DAG、Relative Resize/Native Transient Resource Pool 候选原子替换；Stage/Pass 裁剪必须不误删 SideEffect/Graph export。使用当前 R4 正确的 NativeGraph/Sync2 优先，不把旧 Recorder/ExecutionProgram 再实现一次。第 31 章 Upload Ready→Graph import state/memory/queue family 同步也作为受影响 Native 合同，无真实 Mesh/Texture 业务时可先用 typed upload oracle。
