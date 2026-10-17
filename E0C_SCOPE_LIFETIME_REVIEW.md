# E0-C：Scope 管理异步 Command 寿命评估

日期：2026-10-18。性质：源码调查、隔离原型与初步成本比较；**没有修改生产路径，没有启动 P1–P5/E1–E5**。

## 1. 结论与边界

**可以让 `stdexec::simple_counting_scope + spawn` 替代 RenderServer 自己拥有连接状态的职责。OperationState 本身仍然存在，但它的分配、启动、完成后销毁可交给库。CPU 路径获得有条件 GO；全体 Native Command 的生产切换仍为 PARTIAL。**

这次原型没有把 Pending 改名后继续保存 `void* operation + destroy`：Candidate S 的调用侧确实不再写逐请求 `connect_result_t`、`new Operation`、销毁回调或独立 stop source。业务 Sender 内部必要的 Operation 类型没有删除，也不应删除。

最终交付记录仍需保存来源、拥有型结果和队列节点。这是 IPC 交付责任。Scope 不处理请求身份、Outbox 背压、业务容量、GPU Ticket、回调代码卸载或业务错误优先级。

| 候选/问题 | 本轮判断 |
|---|---|
| S：simple_counting_scope + spawn + 单向结果记录 | **GO（隔离 CPU 寿命/终态可行性）**；建议后续生产候选 |
| C：counting_scope + spawn | 测试通过；并未发现其把已生成 unknown 改为 stopped，但当前并不需要额外 Scope stop 包装 |
| F：spawn_future + 结果观察者 | **NO-GO 作为默认 IPC Command 模型**：增加观察状态和分配，观察者取消可先于生产者的 unknown 结果 |
| 用 scope.join 替代所有 callback/publisher 尾栈保护 | **NO-GO**；已复现 join 完成而生产者尾栈仍活动 |
| 不改 Native GPU 算法而移交执行状态 Owner | **PARTIAL**：真实 Sender 组合编译、桥接形状 ASan 原型通过；真实 GPU 集成未运行 |
| 顺便删除 CompletionPost/CpuResume/CpuPreparation | **未获证明**；不是 Scope 所有权迁移的先决条件，也不是其自动收益 |
| 性能等价/改善 | **PARTIAL**；S 增加局部分配，尾部不优于基线，保留约 16 ms 离群样本 |

对“只保留必要的 IPC 结果交付机制”的准确回答：可以删除自定义的**执行状态 Owner**；当前仍有 CPU→Owner 的中间继续执行桥及其尾栈清偿，不能把它们误当作执行状态所有权一起删除。也无需为了使用 Scope 先引入 OwnerScheduler。

## 2. 精确输入、保护与实际环境

- 调查/报告父提交、检查时远端：`a241cfc633909f963f9353009aff849142bf033f`。
- 对应生产源码：`5288a759dd5fa3565fe7eedeb208d26a1d57d25e`；此后两次提交只增加 E0/E0-B 报告。
- 隔离 worktree：`E:/RenderV2Rebuild/E0C/source`，分支 `codex/render-stdexec-e0c`。
- 附件 SHA-256：`550d1176996f76c0010122a7db87ece9d4f6c995f50d8b2755558dd22e8ad5cf`。
- stdexec：安装标识 `0.10.0`，上游 `7d704575019a1405b2067c5e47265245c55ca66e`；包 SPDX、全部 stdexec/exec 安装头哈希归档。
- lux-cxx：`bf779515a120350c7c5412c366eea73afdf59ccf`；toolset：`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`；均干净。
- MSVC 19.44.35226，Windows 11 build 26200，i9-13900H，14 cores/20 logical processors，宿主 CPU Scheduler 两 Worker。不固定核、不修改电源/优先级。
- Legacy manifest 719 blobs、原工作树 settings/Graph.hpp、V12 用户 RenderServer.hpp 的三个 SHA 保持原样；详见 `evidence/protection.json`。
- Core/IPC/Render/Native 57 个公共源头与 Release/Debug/ASan 三套现有 SDK 逐字节一致。使用既有已验证 DLL，归档其哈希；这不是重新构建生产 DLL 的资格。
- `ValidateTrackedSnapshot.cmake` 在精确基线上通过。本提交白名单只有本报告；生产、测试、旧报告、安装前缀均未改。

## 3. pinned API：实际编译结果

依据实际安装头，并与锁定的上游版本核对：

| API | 本轮行为/编译证据 |
|---|---|
| simple_counting_scope + spawn(just()) + close + sync_wait(join) | `api.cpp` 编译运行通过 |
| counting_scope.request_stop | 编译运行；S/C 全部已生成错误通道比较见下文 |
| spawn_future | 启动、单次观察、放弃、观察者停止均实测 |
| exec::async_scope | 旧扩展 `spawn/on_empty` 编译运行；不混用新 Scope API |
| get_allocator 环境属性 | 精确查询类型编译通过；未设计池或自定义分配框架 |
| materialize | 能规约实际终态，但 pinned 安装头存在可复现的 include 顺序问题 |
| upon_error + upon_stopped + then | 相同六类终态通过；当前单一 C::Response 合同可使用此组合，避免依赖 materialize |

Scope/spawn 属于 C++26 Execution 路线的库设施，当前使用的是 NVIDIA 在 C++20 下提供的 `stdexec` 实现，并非声称 MSVC 的 C++20 标准库已提供它们。`exec::materialize`、`exec::async_scope` 是扩展。标准状态参见 [WG21 P3815R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3815r0.html)；接口兼容仍以 pinned 源码为准。

源码定位：[__spawn.hpp](https://github.com/NVIDIA/stdexec/blob/7d704575019a1405b2067c5e47265245c55ca66e/include/stdexec/__detail/__spawn.hpp) L77–164：分配、connect、try_associate、start；完成时先销毁/释放状态，再释放临时 association。库内部也使用擦除完成函数；消除的是 Lux 手写管理，不是所有间接调用。

两个必要的负例：

1. `spawn(just_error(1), token)` **编译失败**：spawn 的 Receiver 没有通用 set_error。不能把任意 Command Sender 直接丢入 spawn。归一后的精确签名为 `completion_signatures<set_value_t()>`；实际 connect/spawn 模板也已实例化。
2. 先 include `exec/any_sender_of.hpp` 再 include `exec/materialize.hpp`，pinned `materialize.hpp:205` 对 `transform_completion_signatures` 的不限定查找失败。最小复现与编译日志保留；先 include materialize 的原型可编译。**未修改第三方库**。生产建议优先使用已经验证的标准组合链，不以强制全仓 include 顺序掩盖问题。

当前 `AsyncCommandHandlerFor` 限定一个 C::Response value 签名，且 Response 可从 `unexpected<RenderError>` 构造。因此最小规约可写成：

```cpp
// 概念代码；结果记录及准入必须先取得，post 只入现有队列。
auto normalized = std::move(sender)
    | stdexec::upon_error(mapErrorToResponse<C>)
    | stdexec::upon_stopped(makeCancelledResponse<C>)
    | stdexec::then([record](C::Response response) noexcept {
          postResult(record, std::move(response));
      });
stdexec::spawn(std::move(normalized), scope.get_token(), environment);
```

`CommandFailure{outcome_unknown=true}` 必须映射为 `kOutcomeUnknown`；Response 本身的 expected failure 不得转为成功。`exception_ptr` 分支沿用 terminate 策略，已实例化编译，不以执行 terminate 测试充当恢复能力。普通堆 OOM 仍 fatal，不新增伪造的可恢复分配错误。

## 4. 实验设计与真实覆盖

外部 `prototype/main.cpp` 包含两种基线，不能混淆：

- 大量受控反序/尾栈行为用精简 BaselineRecord，208 B，便于控制；它**不是生产 Pending 的尺寸或性能**。
- 正式成本表用真正的 `CommandBinding::bind`、未修改的 `CommandDispatch.cpp`、`PendingSlots`、`PendingOperation`、`CompletionQueue`。预热 StableSlotMap 页；Handler 使用同一个具体 CpuScheduler、同一 `{1,2,3}` owning vector 求和工作。它仍不是完整 RenderClient/RenderServer/GPU 请求测试。

S/C/F 均拥有独立的结果记录，Worker 只发布至唯一原型 ReadyQueue，Owner 才读取结果或访问真实 ResponseOutbox。ReadyQueue 复现现有短锁 intrusive FIFO、先摘节点、原 IPC notify；它在隔离可执行程序中代替原队列，**未给生产新增队列，也不是直接运行生产 CompletionQueue 的候选实现**。

已覆盖：

- Baseline/S/C/F：0/1/16/128/129/1000 在途，反序终态、一次析构/一次结果；S/C/F 的成功、RenderError、known/unknown CommandFailure、stopped、expected failure。
- S：1000 同步结果、1000 双 Worker 结果、无效 CPU Scheduler、预先请求停止、Host 已停止后的调度；错误必须终态清偿。
- 显式最大 1000 的原型准入；第 1001 次拒绝；关闭后不接纳。该数值只是实验上限，不是建议的生产容量。
- 真 IPC：两个 Requester 的 correlation 都为 7，经真实 ResponseOutbox 返回各自的 11/22；Requester 关闭时任务仍存活，完成后由真实 Outbox 清掉失效来源；取出的结果晚于角色析构。
- 结果发布尾栈冻结、外部 producer terminal 后尾栈冻结、旧形状 CompletionPost 中间回传冻结。
- Native 真实 `GpuWorkSender<RenderResult<int>>` 与 `ReadbackSender`：在只借用 CompletionPost 与 stop token 的 Env 下归一并 spawn，**compile-only 通过**，没有修改 Native→Runtime 依赖。

未覆盖/不能宣传为通过：真实 GPU Submit/Fence、DeviceLost、部分提交、Readback 数值、跨 DLL 动态卸载、全部生产 Handler 与 Module Pin、真实 RenderClient 的完整预算/恢复、IPC 长期背压性能。CPU 测试中的 CommandFailure 只验证错误保存，**不是模拟 GPU 来取得 GPU 资格**。

## 5. Scope 与结果、停止、尾栈的准确关系

### 5.1 生命周期所有权

```text
RenderThread 接纳（尚未关闭；预留结果及容量）
  → Handler 生成 Sender
  → spawn 分配/连接/启动，Scope 追踪
  → Sender 的真实终态
  → normalize 构造拥有型结果并发布独立节点
  → spawn 销毁 OperationState、释放 association

RenderThread 从同一 ready queue 取结果
  → ResponseOutbox 接管拥有型消息
  → 发出 / WOULD_BLOCK 重试 / 断连释放
```

结果节点必须独立于即将销毁的 Sender 闭包。生产者发布后只持队列/宿主等外部寿命借用，不再访问节点；Owner 可能在 publish 返回前删除它。Env 应保存 stop token/CompletionPost 的轻量值，避免 Operation 析构期间再通过已经移交的结果节点查询它们。

### 5.2 Stop 来源

当前源码搜索中，逐 Pending 的 stop source 只在 `ServerLoop.cpp:111–127` 的整个 Server shutdown 中调用 request_stop；没有 RequestId cancel/deadline 消费者。因此可以评估一个 Server stop source，减少 N 个源和 shutdown 的 SlotKey 快照/遍历。未来独立取消业务若出现，需重新评估，不能假定 Scope 提供逐请求寻址。

**stop 的调用线程必须保持 RenderThread。** 外部 `RenderServer::stop()` 仍只置位/唤醒，不能直接从外部线程调用 shared stop source，令 Native `work.cancel()` 跑到错误线程。

C 的 `__stop_when` 在 pinned 实现中传递/融合 stop token；它不是可停止的 continues_on。实测 C 在 scope.request_stop 后仍完整转交已生成的 unknown 错误。不能拿 E0-B 的 continues_on 反例直接否定 counting_scope。选择 S 的依据是无需重复 stop 包装、更小状态、业务终态责任更明确。

F 的独立观察者取消是不同问题：本次控制生产者不结束，停止 Future 观察方，先得到 stopped；随后生产者仍返回 unknown。若把前者当作 IPC Cancelled，将错误承诺副作用已取消。丢弃 future 则请求生产者停止，但不会替 IPC 保留结果消费者。因此 F 不适合已知唯一 IPC 出口的默认模型。

### 5.3 三个有区分力的尾栈结果

1. **最终 normalize/post 的尾栈**：Worker 在 Queue.push 的 notify 后被冻结；Owner 已拿到结果，Scope join 仍未完成。解除冻结后才 join。这一部分可由 Scope association 覆盖。
2. **任意 producer 在 terminal 返回后的尾栈**：scope 已 join、Operation 已析构、结果已消费，producer 仍停在调用 terminal 后的代码。标准终态允许接收方销毁操作，**不保证所有调用者代码已退栈**。
3. **中间 CompletionPost 桥**：224 B BridgeRecord 不保存 Operation 指针/destroy/stop；spawn 唯一拥有 BridgeSender Operation。Worker 发布中间继续执行后冻结；Owner 执行 continuation、发布最终结果、scope join；Worker 的中间 publication tail 仍在。ASan 通过，但队列/发布者计数必须活到它退出。

第三个结果说明一个新的、较小的拆分可能：**保留非拥有的 continuation 交付字段，并不等于保留自定义 OperationState Owner。** 可以先移交 Owner，之后再独立讨论是否值得删除 CompletionPost。它不是“再造一张 Pending Owner 表”的许可；本原型没有第二个 Op Owner，也没有新 Scheduler。

`completion_publishers` 当前同时覆盖中间和最终发布，不能在整体迁移时一次删除。Code Pin 也不能仅放在 spawn-owned state 中，就假定其析构后 producer DLL 的 terminal-return 指令还安全。需要已有宿主 join/模块保活规则或实证过的尾栈保护；本轮未做 DLL 卸载实验。

### 5.4 非阻塞停服

推荐候选顺序：Owner 停止接纳 → close Scope → 在 Owner 请求停止 → 持续泵送 Native callbacks/CPU ready/Outbox → 观察 Scope join → 清偿 ready/outbox → 等全部外部发布尾栈 → 按原依赖释放角色、Handler、Device、Instance。

原型只在停服保留一个栈上 join Operation。其 Receiver 环境给 `inline_scheduler`，回调置完成标记并调用现有 IPC notify；等待方继续处理队列。**不需要 OwnerScheduler、等待线程或第二队列。** join Receiver 本身也遵守置位后不再访问可销毁局部状态的约束。

不能在仍依赖 RenderThread 的工作结束前阻塞 `sync_wait(scope.join())`。T0 的 sync_wait 仅用于没有此依赖的简单片段，不能照搬到 ServerLoop。join 也不等于结果已送达；同步 Sender 可以先令 Scope 归零，结果仍在队列或 Outbox 中。

`close()` 后的 spawn 会分配/connect，然后因 association 失败而销毁，既不 start 也不产生 Reply。实验明确复现。因此 Admission 与 close 必须在同一 Owner 安全点串行，接纳后不得落入这个分支。

## 6. 真实 Native 接缝与所有消费者处置

带行号的完整源码快照、全仓消费者搜索见证据 `source-evidence/`、`consumers.txt`、`lifetime-consumers.txt`。下表不把同名标准 Operation 或第三方对象误算作删除对象。

| 对象/字段 | 实际消费者 | 可删除部分及前提 |
|---|---|---|
| PendingOperation / PendingSlots / CompletionId | Deferred.hpp；CommandDispatchContext.hpp；CommandBinding.hpp；CommandDispatch.cpp；ServerImpl.hpp；ServerLoop.cpp；ServerCompletion.cpp；AsyncCost 的统计 seam | scope 可替代 Op Owner、stop 集合、身份遍历；结果节点及必要中间回传仍存在。先全量迁移真实 Handler，再删 Owner 表，不能保留第二张同义表 |
| operation_state_ / destroy_operation_ / ownOperation | CommandBinding async 分支、CommandDispatch、Pending 析构 | spawn 直接替代；同步分支不进入 Scope |
| CommandBinding::Receiver<C> | bind 的异步 thunk；其 Env 是 Native/CreateEffect 的桥来源 | 完成规约可用组合链；删除手写 Receiver 后仍要传正确 Env，不复制 Command 目录 |
| 每请求 stop source | Receiver Env、ServerLoop shutdown | 当前只有 Server-wide stop；候选用一源。Native OnStop 的 Owner 线程要求不变 |
| result_message_/ResponseRoute | Receiver、drainCompletions、ResponseOutbox | 留在独立交付记录。Route 保留 RequesterId+RequestId，不可只用 correlation |
| CompletionQueue/ready_next | Deferred、ServerImpl、CommandDispatchContext、drain、cost seam | 保留唯一通路，可改节点载体；不要再加 scope 专用队列 |
| CompletionPost/GetCompletionPost/CpuResume | Core WorkSchedule；Render AsyncWork 别名；CommandBinding Env；Deferred/CommandDispatch；CreateEffectSender；GpuWork::start；Transfer::start/CpuPreparation/resumeCpu | Scope 不能直接删除它们；可借结果/桥记录，不必借 Op Owner。彻底删除须先迁 CreateEffect 和 Native 中间返回桥 |
| completion_publishers | Deferred::post、CommandDispatchContext、ServerImpl、ServerLoop shutdown | 最终 post 部分可能由 scope 覆盖；中间 post 和外部尾栈未覆盖，暂留 |
| CreateEffectSender::Operation | ServerEffect、内置 CreateEffect Command | scope 可以拥有它；compile/Owner adopt 仍必要。替换其手写 CpuReceiver/adopt 是独立的组合迁移，本轮未编译完整新 CreateEffect |
| Transfer::CpuPreparation | Transfer.cpp/.hpp；借 SessionUploads 的 CpuSchedule | 仍表示每 Chunk CPU 准备；不把其 reset 移进自己的 then 完成尾栈。Scope 并不自动消掉这个内部子操作 |
| GpuWorkSender/GpuWork | TextureCommandHandler set/region、MeshCommandHandler set、MaterialCommandHandler set；Transfer | 保留真实 Source/候选/Transfer/Stop；外层 Op Owner 可迁，但需 GPU 集成验证 |
| ReadbackSender/ReadbackOperation | ServerCompletion readBackOutput；ExecutionContext；SubmissionQueue callback | 无 CompletionPost 查询，可直接编译进 Scope；仍须真实 Context/Ticket 生命周期验证 |
| CpuSchedule/CpuSender/WorkSender | WorkSchedule.hpp；AsyncWork 别名；ServerImpl/RenderServer/ServerEffect；DeviceSession、SessionUploads、Transfer；Core WorkScheduleTest、Render resource CpuFixture、Runtime ServerFixture/async_tail/Offscreen/TextureProgress/AsyncCost | E0-B 具体 CpuScheduler 删除路线仍独立成立。本轮不把其分配收益归给 Scope |
| CommandRegistry/ResponseOutbox/DeviceSession/SubmissionQueue/Retirement | 真实业务身份、IPC 背压、GPU 资源与完成 | 保留。不能用 Scope 代替真实 GPU 完成或资源释放条件 |

搜索还命中 Engine AssetLoadSender、ScriptAssetAccess，它们使用具体 CpuScheduler；Core ExecutionRuntime/Scheduler 定义该 Scheduler。**不是本次应删除的 CPU 类型擦除消费者**。Core WorkScheduleTest、Render CpuFixture 和 async_tail Provider 是接缝测试消费者，后续迁移需保留它们各自不变量；README/构建引用随真实删除更新。本轮没有改它们。

Native 源码检查：

- `GpuWork.hpp:141–163` 在 terminal 前完成 publish/unknown 判定、reset Native work、移出 receiver/response；terminal 后不再读取 self。Scope 不能更改 Success 的真实 GPU 条件。
- `Transfer.cpp:288–300` 在 Owner `resumeCpu` 中 reset CpuPreparation，然后推进 Copy。不是通过新增 continues_on 就能安全移动这个销毁点。
- `Readback.cpp::finish` 先 unlink、清 Context、reset callback，然后 terminal；注释明确允许 terminal 销毁操作。
- `Submission.cpp::dispatchCompleted` 先摘节点、保存后继和函数、解除 armed，再调用。仍由真实 Ticket 前沿决定运行。

这些源码形状支持后续集成候选，不等于已经运行 GPU。两种迁移策略都可研究：CPU-only 暂用 Scope、Native 暂旧 Pending 共用一队列；或以非拥有 BridgeRecord 让全体外层 Op 都由 Scope 管。后者减少双所有权模式，但不能仅凭本次 compile-only/CPU 桥测试宣布完成。

## 7. 容量与最坏在途：不能把 Scope 当预算

检查真实基线发现：Pending StableSlotMap 动态增长；当前 IPC 使用分段增长 Ring；`request_work_budget` 是每 Tick 派发量，不是总异步容量。没有找到覆盖“运行中 + ready + Outbox + 保留 payload”的 Server 总额度。

因此本轮只证明**可以在 spawn 之前拒绝**，没有证明生产已经有充分总量背压。未来迁移至少需复用/明确一份准入计数与字节预算，承接至 Outbox 真正交付或释放；不能在 scope count 归零或 ready 入队时就退款。全背压逻辑不属于 Scope，不建议为它引入新的通用 Manager。

原型最大 1000，输入为小 vector；测试最大节点数和拒绝行为可复核。原型在结果从队列取出时减少 inflight，因此它**不构成长期 Outbox 背压下的生产内存上界证明**。生产切换前必须补此门禁。

## 8. 尺寸、分配与时间

64-bit Release 实测：simple scope 16 B，counting scope 40 B；生产 PendingOperation 272 B；S 结果节点 160 B；兼容旧中间回传字段的 BridgeRecord 224 B。后两个均是隔离候选，不是已承诺安装 ABI。

成本表使用真实生产 Pending 路径作 baseline（不是 208 B 精简控制夹具）：

| 量 | baseline | S | C | F |
|---|---:|---:|---:|---:|
| 预热后 EXE new/请求 | 1 | 2 | 2 | 3 |
| 本次计时 EXE 分配 bytes/请求 | 160 | 376 | 472 | 704 |
| DLL/CRT 内部分配 | NOT_MEASURED | NOT_MEASURED | NOT_MEASURED | NOT_MEASURED |
| 结果入队次数/请求 | 1 | 1 | 1 | 1 |
| 连接状态的手写 Owner | Pending+destroy fn | 无 | 无 | 无，但多一个 Future 观察状态 |
| 结果存储 | 预热 StableSlotMap 页 | 独立记录 | 独立记录 | 独立记录 + Future 生产/观察 |
| Scope/stop 控制 | 每请求源 | Scope + 一 Server 源 | counting scope（实验另有 Env token） | simple scope + 观察方 token |

baseline 的预热 Pending 页分配不在 160 B 中；160 B 是本例 connected CPU operation。S 的 376 B 包含 160 B 结果节点和 216 B 库 spawn 状态。不能将 272 B Pending 与 160 B 结果节点相减，就当作完整请求节省量。Record 的额外 heap 是明确代价；未加池或额外缓存来美化数据。

实验：10 个独立进程，每进程每候选预热 100、正式 1000 次，候选顺序轮换。每次计时覆盖 start/CPU 求和/结果入队/Owner 消费和本次 close/join 清偿；输入 vector、Host 构造、预热页在计时外。结果 observer 替代 IPC 收取；正式成本计时不执行 Outbox flush/GPU。**每样本 join 与生产长期 Scope 不同，不能据此判断完整 Server 速度。** 原型用 yield 等待，没有把既有 Server 空闲等待计入纯队列成本。

各 run 分位数的中位数，单位 µs；max 列为所有原始样本最大值：

| 候选 | p50 | p95 | p99 | 全部 max |
|---|---:|---:|---:|---:|
| baseline | 3.60 | 6.75 | 14.40 | 261.7 |
| S | 3.15 | 10.45 | 17.25 | 15928.5 |
| C | 3.20 | 11.10 | 18.25 | 189.6 |
| F | 3.30 | 10.00 | 16.25 | 14584.7 |

逐 run 配对 p95 差值的中位数：S +3.95 µs、C +4.45 µs、F +3.50 µs。不是简单将上表中位数相减。全部 40,000 个正式样本、预热以外离群值、每进程 stderr 和逐 run 统计均保留。

总进程 CPU 时间（四模式及打印共同计入，以一逻辑核 100%）中位 106.4%，范围 33.4%–233.9%。短进程 GetProcessTimes 粗粒度很大，不能分摊为候选 CPU 成本或空闲 CPU；这些分项 **NOT_MEASURED**。DLL 分配也没有伪填 0。没有运行 VTune/ETW 归因，S/F 约 15 ms 尾部原因未知，**性能 PARTIAL**。不宣称更快、无回归或完整请求零分配。

## 9. 复杂度与净代码变化判断

本轮生产实际变化 **0 行、0 类型**。下列是后续候选的结构账，不是已提交的净删除量：

- 删除一套手写 per-request connected operation Owner、void*/destroy function、逐请求 stop 源与 stop 身份快照。
- PendingSlots/CompletionId 若没有业务查询消费者可删除；结果节点/队列仍需唯一持有规则。
- `CommandBinding::Receiver<C>` 一个手写 Receiver 模板可换成终态组合；增加一个 shutdown JoinReceiver/一次 join operation。标准库内部 Receiver/Op 实例并未消失。
- 采用非拥有 BridgeRecord 时，continuation/resume/result 三字段暂留；这是一个节点形状选择，不是另增 Operation Owner。
- 不新增 Scheduler/TaskManager/CompletionQueue/FutureRegistry；Scope+共享 stop 是聚合库状态。

按当前源码函数块粗估，所有权/逐请求 stop/辅助桥接 API 可删约 70–110 非空行，Scope/终态规约/join/admission 和结果移交需约 70–120 行，净约 **删除 40 到增加 50 行**。不是生产 patch 或编译实例计数，**不作为简化已完成的证据**；精确差值须从后续完整迁移 diff 计算。若为同一职责再增加 Owner 或另一完成框架，即使 Scope 编译通过也应停止。相对 E0-C 的价值是责任删除已经可实证，而非承诺巨大行数下降。

## 10. 验证矩阵与可复现材料

| 项目 | 结果 |
|---|---|
| RelWithDebInfo 原型 | all 两轮，第二轮 no-work，CTest 2/2 |
| Debug 原型 | 同上，2/2 |
| MSVC ASan 原型 | 同上，2/2；`alloc_dealloc_mismatch=1`，无 ASan 诊断 |
| `/EHs-c- /D_HAS_EXCEPTIONS=0` 外部原型 | all/no-work/CTest 2/2；生产仍保持原异常策略 |
| 真实 Native Sender Scope 组合 | MSVC C++20 `/GR-` compile-only；没有链接执行 GPU |
| 两个负编译片段 | 均按预期失败：raw error spawn、materialize include 顺序 |
| 真实 IPC/Outbox 来源/断连 | 隔离测试通过；不等于完整 RenderClient/GPU qualification |
| 基线 snapshot/719 blobs/用户 3 文件/历史保护 | 通过，仅新增本报告 |
| 全生产矩阵/真实 GPU/Validation/SyncValidation | NOT_RUN；没有改生产 |
| 跨 DLL 卸载/TSan/Linux/Android/Clang/GCC | NOT_RUN；当前环境/VS LLVM 路径没有发现可直接调用的额外编译器 |
| Root TOOLCHAIN、旧 Debug 失败、旧性能 | 历史状态原样：Root BLOCKED、C1–C5 Debug graph.edit 失败、性能 PARTIAL；本轮未重跑或改判 |

复现入口（源码树外，路径在脚本中明确）：

```powershell
pwsh -File E:/RenderV2Rebuild/E0C/run.ps1
pwsh -File E:/RenderV2Rebuild/E0C/run.ps1 -Config Debug -Mode d
pwsh -File E:/RenderV2Rebuild/E0C/run.ps1 -Mode asan
pwsh -File E:/RenderV2Rebuild/E0C/run.ps1 -Mode noeh
# 每个配置：cmake --build <build> --target all -j 4 -- -k 0 两轮
# 然后：ctest --test-dir <build> --output-on-failure -V -j 1
pwsh -File E:/RenderV2Rebuild/E0C/compile-extra.ps1
py -3 E:/RenderV2Rebuild/E0C/measure.py
py -3 E:/RenderV2Rebuild/E0C/metrics.py
py -3 E:/RenderV2Rebuild/E0C/collect.py
```

原型、脚本、源码行号快照、编译命令、依赖头与 DLL 哈希、原始样本及失败尝试均在证据包。初次实验的 allocator 引用类型断言、initializer-list 与 noexcept emplace 不匹配、Windows max 宏、基线空 scope 测试断言、嵌套 CpuReceiver 环境返回类型推导均在原型修正；Native 单独 cl 初次遗漏 `/Zc:__cplusplus`、`/Zc:preprocessor` 的失败日志保留。materialize include 顺序为仍存在的 pinned 库问题，未冒充已修库。`empty_env` 弃用等编译警告原样保留。

证据包：`E:/RenderV2Rebuild/archives/a241cfc633909f963f9353009aff849142bf033f/E0C-scope-lifetime-evaluation.zip`。

证据 SHA-256：`01b36776e7720e84bf8a68b9ecc4de4220b2363babf766bf6abaf10d94859aa2`。包内 `SHA256_INDEX.json` 逐文件记录哈希和大小；可复制解压，依实际 SDK 根调整脚本路径重跑。评估程序与负例是外部原型，不安装到生产模块。

## 11. 后续建议与明确停止点

建议下一次若授权生产，只围绕“Scope 接管连接状态 Owner”制定有界迁移：保持唯一 CommandRegistry、唯一结果队列和 Outbox，先明确总在途/Outbox 字节额度，stop 仍由 Owner 发起；保留 Native 中间桥及必要尾栈保护，使用真实 GPU 验证即时销毁的接缝。不要把 Scope 引入、OwnerScheduler、每 Chunk 状态复用和 Transfer 改写绑在一起。

CPU-only 证明足以改变“Pending 必须永久拥有每个 OperationState”的默认结论；**不足以授权立即删除全部 Pending 状态或宣传 Native 全量 PASS**。本轮到 E0-C 研究结束，生产迁移等待用户裁决。
