# RenderServer × stdexec：执行所有权收敛

基线 `6c9e95fc247a82e90430e6b503060896e735516f`；stdexec 0.10.0，上游 `7d704575019a1405b2067c5e47265245c55ca66e`。施工阶段的测试与选择以最终结果报告为准，本文不授予历史或当前测试 PASS。

## A：直接 CPU Scheduler

宿主仍唯一拥有 `execution::ExecutionRuntime`。RenderServer 保存其具体 `CpuScheduler`，DeviceSession 的上传配置保存同类型借用。CreateEffect 和 Transfer 的具体 connected state 直接连接 `schedule(cpu) | then(work)`。

删除的是 WorkSender/CpuSender 的额外类型擦除和内部动态 Operation 包装，不是实际工作所需的 connected state。`EExecutionError` 只在两个实际 Render 边界映射到 `kCpuScheduleFailure`；停止仍分别遵守原 CreateEffect/Transfer 合同。

Native `resumeCpu()` 继续在原 Owner 完成通知消费点回收 CpuPreparation。CPU Prepare 不访问可变 Server 目录；CreateEffect 候选仍只由 RenderThread 采用。没有新增 Scheduler、线程或队列。

## B/C 的选择与必须保持的不变量

实际比较 S（simple_counting_scope + 一个 Server stop source）和 C（counting_scope 内置 stop）。两者均完成真实生产 Release 146/146；C 另完成施工 ASan 146/146。最终选择 C，仅保留 counting_scope。它把关联计数和停止源保持在一个库对象中，删除 S 额外需要的 Server stop source、CommandDispatchContext stop 引用、commandStopToken 转发和 CompletionEnvironment stop 字段。决定依据是完整 Native stop/unknown、同步终态及回调尾栈行为，不是声称状态尺寸必然更小。S 对照补丁及全部日志外置保留，生产不提供双策略开关。

锁定版本 `__stop_when.hpp::__make_token_fn` 在 Receiver 环境不可停止时直接返回 scope token，只有两个可停止 token 才创建 fused token。生产 C 因此不在 CompletionEnvironment 里额外提供 stop source/token；不能把 E0-C 同时注入两源的原型成本直接当成生产成本。Native 实际收到现有 inplace stop token，三种 Queue 拓扑及部分提交/停止/DeviceLost 用例已完成施工回归；最终资格仍以独立 V 为准。

```text
Owner dispatch
  → 取得结果交付落点
  → Handler 返回 Sender
  → upon_error / upon_stopped 归一为准确 Response
  → then 写拥有型结果、发布现有 CompletionQueue
  → spawn 持有 connected state，终态后库销毁
Owner drain
  → 摘除通知
  → 中间 Native continuation 或最终 ResponseOutbox
```

结果记录只承担 IPC 路由、拥有型结果及就绪节点；不得再保存 operation 指针、销毁函数、Pending 身份或逐请求 stop source。Scope 只管理通用执行寿命，不保证客户端消费或 GPU 资源退休。

`CommandFailure.outcome_unknown` 必须优先生成 `kOutcomeUnknown`；不能把已发生提交的错误转换为确定取消。业务 expected failure 原样交付，异常通道 terminate。同步 Handler 继续原地 Response，不进入 scope，不增加结果记录或异步分配。

## 三类寿命不能混淆

1. **connected OperationState**：spawn/Scope 管理。同步终态可以销毁状态，生产者不能在 terminal 后读取状态。
2. **结果交付**：结果发布后由 Owner 消费；回复可因 Outbox 背压晚于操作完成，也可晚于 Server 被客户端持有。Scope join 不代表回复已消费。
3. **外部 Producer 尾栈和代码**：Scope join 不覆盖 terminal 后仍执行的 DLL/Host 回调，也不自动覆盖 Native 中间 CompletionPost 发布尾栈。保留现有 publisher 计数与宿主 join/代码 pin 义务。

停服关闭 Owner 准入并请求停止后，RenderThread 必须继续推进原 CompletionQueue、Native Queue 前沿和回复。禁止对依赖 RenderThread 继续执行的 scope.join 调用阻塞 sync_wait。Scope 与队列清偿后还需保护 publisher 尾栈，再关闭 Responder；Native GPU drain/退休仍在 Device/Instance 销毁前完成。

## 容量与成本的真实边界

原 PendingSlots 已动态增长，ResponseOutbox 和当前 IPC Ring 也不是由 request_work_budget 限制的全局在途字节预算。删除 PendingSlots 不能宣传解决了全局背压，也不能把 Scope 计数当内存限额。本轮分别记录运行操作、已就绪交付、Outbox 和拥有型结果；不凭空增加 TaskManager 或另一套预算状态机。

CPU 适配删除与 Scope 迁移分别验证。Scope 可能以额外结果记录/分配换取更少手工所有权代码；只有生产同工作量样本能评价代价，隔离原型不构成性能 PASS。
