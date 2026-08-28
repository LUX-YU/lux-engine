# Process Wave 0/1 仓库适配修正

状态：本文件是 `lux-process-execution-reference` 在 lux-engine 当前仓库中的实施修正。

冲突优先级固定为：本修正 > 同目录原始设计文档 > 外部参考包中的示例与 proposed 代码。

## 本轮范围

- 只实施 Wave 0/1：Process source topology、`process_execution`、Timer sender 与
  `OperationPort<T>` sender adapter。
- `process_io`、Render sender、Asset/Streaming workflow、coroutine 和 runtime-cardinality
  dynamic fan-out 均不在本轮创建 public API。
- `DYNAMIC_FANOUT_DECISION.zh-CN.md` 保持有效；真实 Model/Streaming consumer 出现前不得新增
  `parallelTransform`、`BatchJoin` 或新的 AsyncGraph。
- legacy `runtime/execution` 只作为行为与测试素材，不迁移其 Runtime、Builder、registry、CPU pool、
  main-loop ownership 或兼容 API。

## 当前仓库适配

- 基线为 `25992bb578ccc7765a0f238c582f62d5424352d1`。
- 当前 vcpkg stdexec 版本为 `2026-02-26`，custom sender 使用 `stdexec::sender_t`，operation state
  使用 `stdexec::operation_state_t`；原始示例中的 `sender_tag` / `operation_state_tag` 不适用。
- `TimerQueueConfig::capacity` 必须由调用方显式提供，不使用参考代码的隐藏默认容量。
- `TimerSender::after()` 的 delay 从 `start()` 计算，以保持 Sender lazy；非正 delay 立即到期，
  超范围 delay 饱和到 `steady_clock::time_point::max()`。
- Timer worker 必须在状态成员与全部预留缓存构造完成后启动；shutdown 不得临时分配。
- `OperationPort` rejection 返回后不得再异步调用 completion。adapter 必须兼容同步 completion、
  同步 rejection completion 和无 completion 的 rejection，并保证 receiver 只完成一次。

## 资格状态

本轮完成后只能标记 Process Wave 0/1 Freeze Candidate。File IO 是 Wave 2；动态 fan-out 仍为
deliberately deferred decision，不因 Wave 0/1 完成而冻结。
