# RD3-P3 — Vulkan Compiler / Graph / Executor / Context 与完整 Target（DRAFT，NOT_AUTHORIZED）

## 目标

把现有 F4/F5 C9–C13 Native Graph Code、Invocations、Slot/Sync2/Queue、R4 RAII 和 Win32 WSI 的**实际责任**迁移到 `backend/vulkan/graph/{Compiler,Graph,Executor,GraphExecutionContext}` 与 target 后端，不保留另一层长期 NativeGraphInvocations/RuntimeNativeVariant 作为权威。Compiler 产出 immutable data；Executor 每执行任务使用 bounded Context；一次执行完整的当前 Target 最终画面。

## 必须验证的行为

- Pass 顺序/读写/barrier/transfer/alias、minimum `vkCmdBindPipeline/vkCmdBindDescriptorSets`/PSO recipe、实际同 Shader 多 Owner 绑定与 GPU 输出。
- 1 Scene/4 Views、2 Scenes/4 Views 共用 immutable Graph 但独立 Context，Scene Pass only reuse when proven，2/3 FIF, imported resource identity/Ready/ticket、非同一 memory alias。
- 单 View+单 Target execute 全输出、同一 Target N View 一图显式 Composite/Overlay、Offscreen/Surface+双 Window/resize/minimize，最终 Target 可 readback，**禁止图外隐藏 graphics Pass**。
- 某 View/Graph 在不同 Target signature/size 下有必要的 native variant，旧 Context 保留直到 GPU/WSI done，创建失败 last-good。
- Actual Context slots 按活跃 Job 的有界资源数创建，不为每 Native variant 按全局最大 Views 满额预创建 GBuffer/HZB/Transient。
- GPU DeviceLost/partial submit/queue family/host readback/Presentation fence 不伪造 completion，不能用 frame serial 替代 Queue Ticket。

## 性能及施工限制

锁定 native benchmark 工具/Shader/binary 与 AA/BB/AB 统计；固定执行首方 0 heap、Native Graph 编译/Layout identity cold evidence、VRAM peak/used/reserved 多尺寸报告，不接受“结构更优雅”代替性能债。必要修正提前明示 CORE/Transport/F2/F3 的 impacted File DAG，不静默改已验证结构、不引入 `GraphInstanceManager/RenderTargetManager/FrameExecutorManager`。

## Gate

P2 PASS/user authorization→I（生产+测试+冻结 perf protocol）→独立 clone V（Normal/Full ASan、实际 Vulkan GPU Validation/SyncValidation 0 error、真实 oracle 与压测）→STOP；旧 F4/F5/F6.b debts 保留直到符合原资格的正式 closure。
