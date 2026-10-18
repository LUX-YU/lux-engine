# Execution 收敛：消费者与删除边界

基线：`6c9e95fc247a82e90430e6b503060896e735516f`。本文为本轮施工材料，状态以最终结果报告为准。

## 已核实的模块边界

| 候选及源码 | 真实消费者 | 处置与前提 | 风险/净变化 |
|---|---|---|---|
| `modules/core/execution/include/lux/engine/core/execution/WorkSchedule.hpp`：WorkSender、类型擦除 Operation、绑定工厂 | Render `AsyncWork.hpp`；生产最终消费者为 ServerEffect 和 Transfer::CpuPreparation | DELETE：直接 schedule(CpuScheduler) + then；EExecutionError 在 Render 边界映射 | 删除擦除分配；仍保留各自具体 connected state；不改变 Native resume 回收点 |
| 同文件 CompletionPost/CpuResume/GetCompletionPost | CreateEffect CPU→Owner 采用、Transfer CPU→Owner 继续提交、GpuWorkSender 环境查询 | KEEP，独立窄头 `CompletionPost.hpp` | 非拥有回传合同；标准 Scope 不提供 Owner 线程亲和性；不能用 join 替代 publisher 尾栈 |
| `render/resource/.../AsyncWork.hpp` | 上述 Native/Runtime 和 Native 测试宿主 | MERGE/缩减：删除 CpuSender/CpuSchedule 别名，保留 Render 错误和有消费者的桥类型 | 不为外观统一增加另一个 Scheduler |
| `render/runtime/pinclude/Deferred.hpp`：PendingOperation、PendingSlots、CompletionId | CommandBinding、CommandDispatch、ServerLoop、ServerCompletion；Async/AsyncCost/Handlers/GpuReady 测试 | DELETE 候选：spawn 接管 connected state，结果记录保留路由、消息和队列节点 | 必须真实 Native stop/unknown、同步完成及代码尾栈验证；不是重命名 Owner |
| `render/runtime/include/.../CommandBinding.hpp`：Receiver、ownOperation 入口 | 所有注册异步 Handler，包括外部 DLL | MERGE 候选：upon_error/upon_stopped/then 归一终态后 spawn | expected failure、CommandFailure unknown 不能被取消覆盖 |
| `render/runtime/src/CommandDispatch.cpp`、`pinclude/CommandDispatchContext.hpp` | CommandBinding 模板与 RenderServer 分发 | MERGE 候选：删除仅服务 Pending 身份/销毁的辅助函数；保留必要隐藏实现边界 | 公开模板需访问私有 Scope/结果发布；不能为删文件泄漏完整 Impl |
| `render/runtime/src/ServerEffect.cpp`：CreateEffectSender::Operation/CpuReceiver | 内建 CreateEffect | KEEP 当前采用桥；进一步标准组合属于有条件候选 | Worker 只 compileEffect；效果目录发布必须 Owner；本轮禁止另建 OwnerScheduler |
| `render/native/vulkan/src/transfer/Transfer.cpp`：CpuPreparation | Transfer 的 CPU 分块准备 | KEEP 具体 operation 存储，DELETE 擦除调度层 | resumeCpu 回收点不移动到其自身 then 调用栈；逐 Chunk 分配不是 Scope 自动能删除的职责 |
| `render/native/vulkan/.../transfer/GpuWork.hpp`、`src/Readback.cpp` | Texture/Mesh/Material、TestBuffer、Runtime Readback | KEEP | Source 保活、实际提交、Acquire、资源发布、Readback reader、kOutcomeUnknown 均非通用 Scope 职责 |
| Native SubmissionCallback/Retirement/LastUse | 所有真实 GPU 执行和资源释放 | KEEP | GPU Ticket 前沿、跨 Queue 条件、部分提交和物理退休不能用 CPU scope 计数替代 |

## 额外发现：不得混同的 Execution 机制

| 源码 | 消费者与实际职责 | 分类、替代前提及预计变化 |
|---|---|---|
| `modules/core/execution/include/.../TaskScope.hpp`、`src/Task.cpp`、`Task.hpp` | AssetLoad、VfsAssetReadEndpoint、ScriptAssetAccess、ScriptRuntimeSystem、WorldLoadingSystem、Editor app；TaskReporter、TaskId、进度、历史、组停止 | FUTURE：内部 connected state owner 有可替代潜力，但不能删除任务监测/预算/业务组语义。需要全部上层消费者独立授权与验证；本轮净改动 0 |
| `modules/core/execution/src/ExecutionRuntime.cpp`、`Scheduler.hpp` | Render 与 Engine 共同宿主；实际 worker、CPU/Blocking queue、平台等待和 join | KEEP：stdexec schedule 消费已有 Scheduler，不凭空提供已配置线程池；删除会丢失执行资源与停服保证，净改动 0 |
| `modules/core/execution/.../CompletionWork.hpp`、`ExecutionRuntime.cpp` | EditorUiScene、SceneRuntime，以及 installed consumer | KEEP：合并通知、cancel 后迟到 requester 惰性失效、Owner 安全点执行；并非逐 Command OperationState Owner，净改动 0 |
| `modules/core/execution/.../PortSender.hpp` | AssetLoadSender、PortSenderTest；OperationPort 到 Sender 的边界 | KEEP：保留提交同步重入交接，真实 endpoint 没有原生 Sender；用 then 不能替代 submit/callback 竞态。后续可单独研究任务关联头耦合，净改动 0 |
| `modules/core/async_port/.../OperationPort.hpp` | Asset、Script、Scene 外部操作合同及 PortSender | KEEP：typed Value/Failure、容量拒绝及外部提交协议，不等同 Scheduler，净改动 0 |
| `modules/core/async_port/.../OperationInbox.hpp` | 当前仓库搜索未发现实例消费者；已公开安装的模板 API | FUTURE：有容量、key 和 Owner drain 语义。无消费者不证明 stdexec 完整替代；可单独确认外部 API 使用后删除，不趁本轮删除无关 API，净改动 0 |
| `modules/core/task`：TaskGraph/Builder/TaskExecutor/TaskCallable | SceneRuntime、SceneDriver、ECS 调度闭包 | KEEP：不可变 DAG、CSR 后继、资源依赖、CALLER_THREAD affinity、稳定执行存储。不是简单逐请求 spawn；本轮不换 ECS 执行器，净改动 0 |
| `render/runtime/pinclude/ResponseOutbox.hpp` | 同步 Handler 和异步结果 Owner 线程交付 | KEEP：Requester 路由、WOULD_BLOCK 轮转和断连清理。Scope join 不意味着客户端已消费回复；不能删除 |

搜索覆盖 modules、engine、editor 的实际 include 和符号引用；额外候选不能只按 Task/Queue/Scope 名称判冗余。运行中完成队列、IPC Ring、GPU Queue 是三种不同的协议，不合并。

## 逐文件补充依据

以下路径均相对仓库根；完整符号消费者列表另存证据包 `execution-consumers-current.txt`。

- `modules/core/execution/include/lux/engine/core/execution/TaskScope.hpp` 的 `submit()` 先进行 Runtime admission，取得 TaskId/TaskReporter，再建立具体 TaskOperation；`requestStop()`、`settled()`、`join()` 操作整个业务 TaskGroup。消费者包括 `engine/process/asset_loading/src/VfsAssetReadEndpoint.cpp`、`engine/scene/scripting/assets/src/ScriptAssetAccess.cpp`、`engine/scene/builtin_systems/world_loading/include/lux/engine/scene/WorldLoadingSystem.hpp`、`editor/context/include/lux/engine/editor/EditorContext.hpp`、`editor/app/src/LuxEngine.cpp`。标准 scope 可研究接管其操作存储，但不会提供任务历史、进度和准入；本轮不迁移这些业务。
- `modules/core/execution/include/lux/engine/core/execution/PortSender.hpp` 的 `TState` 区分 submit 调用未返回、异步完成和已交付，防止同步 endpoint 在 submit 尾栈访问已销毁状态。生产消费者为 `engine/process/asset_loading/include/lux/engine/process/asset_loading/AssetLoadSender.hpp` 和 `engine/process/world_loading/src/WorldPartitionLoad.cpp`。这是外部 OperationPort 到 Execution 的真实边界，不是重复 CPU Scheduler。
- `modules/core/execution/include/lux/engine/core/execution/CompletionWork.hpp` 的 requester 可以晚于 Owner，cancel 使迟到请求失效；`engine/scene/composition/src/SceneRuntime.cpp`、`editor/app/src/EditorUiScene.cpp` 使用同一合并通知。它没有每次请求的 connected OperationState Owner。删除它必须重新解决合并、Owner 线程及迟到通知，不属于本轮 scope 替换。
- `modules/core/async_port/include/lux/engine/core/async/OperationInbox.hpp` 的 CallbackState/Control 负责有界准入、同步回调交接、带 Key 的结果排队与关闭。仓库没有实例消费者，但这仍是公开模板；若单独批准 API 清理，可研究直接删除而不是新建 Sender Inbox。此处是 FUTURE，不能虚称已由 spawn 接管。
- `modules/core/task/include/lux/engine/task/TaskGraph.hpp`、`TaskGraphBuilder.hpp`、`TaskExecutor.hpp`、`TaskCallable.hpp` 及对应 `src` 管理带资源访问和线程亲和性的重复执行 DAG。`engine/scene/composition/src/SceneRuntime.cpp` 和 ECS 调度消费者需要这些不变量。`PendingTask` 是冷态建图项，不是异步 Command Pending；名字相近不是删除依据。
- `modules/function/render/resource/include/lux/engine/render/resource/Factory.hpp`、`ResourceRegistry.hpp`、`StorageSet.hpp` 的擦除对象是专业 GPU Storage 实例，不是 Execution 操作。`modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/ExecutionContext.hpp` 中 `StorageUse` 的订阅与资源替换重绑定依赖真实 Storage/Graph 生命周期，不能改成 scope association。保留这些唯一资源权威及 Native 退休路径，净改动为零。

本轮没有发现另一项可在不扩大业务范围、且不丢失上述合同的条件下安全删除的独立长期组件。结论不是“Core 已经没有冗余”，而是额外删除尚缺跨业务消费者验证；不以另造适配层替换现有适配层。

`modules/core/execution/include/lux/engine/core/execution/Timer.hpp` 与 `src/Timer.cpp` 归为 KEEP：TimerSender 已经是标准 Sender，其底层有界 deadline heap、取消队列和实际计时线程提供执行资源。消费者包括 `engine/scene/composition/src/SceneRuntime.cpp` 和 ScriptRealDelayProvider 的真实延时路径。scope 不提供计时设施；本轮不替换计时线程或改变 Script 行为，净改动零。

## 已落地的删除

上表 PendingOperation/PendingSlots/CompletionId 与 CommandBinding Receiver 已从候选变为实际 DELETE/MERGE。CommandCompletion 只保留路由、结果、非拥有 continuation 与原队列节点；没有 operation 指针、操作删除器、请求 stop source 或 SlotMap 身份。CommandDispatch.cpp 继续承担公开模板访问私有实现的窄接缝，删除 ownOperation/operationStopToken 等旧 Owner 辅助函数，但不为减少一个文件把 ServerImpl 泄漏到公共头。其余 KEEP/FUTURE 不改变生产路径。
