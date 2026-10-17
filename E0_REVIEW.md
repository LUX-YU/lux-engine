# RenderServer stdexec 收敛：E0 评估

本轮结论：**E1 有条件 GO；E2 PARTIAL；E3 当前 NO-GO；E4 被 E3 阻断；E5 保留现有 Native Sender。** 可以先减少 CreateEffect 的手写异步机制，但不能据此宣称整个 RenderServer 的完成机制可以删除，也不能宣称性能已经改善。

本文件是评估，不是 E1–E5 实施结果。生产代码、生产 CMake、既有测试、公共 SDK 接口均未修改。原型位于源码树外，不接入正式构建。

## 1. 基线与证据范围

- 精确源码及开始时 fetch 后远端：`5288a759dd5fa3565fe7eedeb208d26a1d57d25e`，目标 `codex/render-v2-rebuild`。
- 隔离评估树：`E:/RenderV2Rebuild/E0/source`，分支 `codex/render-stdexec-e0`。
- 任务书 SHA-256：`47193d476edb1c89106fe2a361633334b57e94a69aa59a6f0ee9b23163aec636`。
- 依赖：lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`；toolset `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`；两者工作树干净。
- stdexec 安装包标识 `2026-02-26`，CMake/头版本 `0.10.0`；本机 port REF `7d704575019a1405b2067c5e47265245c55ca66e`。REF 来自本机 port，安装内容另外按文件哈希锁定，不仅依据版本字符串。
- Windows x64，i9-13900H（14核/20逻辑处理器），MSVC 19.44.35226，C++20、`/GR-`。本轮未修改或重新编译上层产品。

原型使用既有 Core Execution 和 IPC DLL。Release SDK 来自 C1–C5 的最终生产 I `60e02674c5ef6bb3a3327f66b4e2761304a64af9`；Debug/ASan 从对应独立构建树重新安装到 E0 私有前缀。该 I 到本轮精确基线的 `modules/`、`cmake/` diff 为空；34 个相关安装公共头与基线逐字节一致。没有使用未提交的主工作树头补全依赖。

历史资格保持原样：C1–C5 Release 146/146、Debug 145/146（`graph.edit` 失败）、ASan 146/146 来自原独立报告，**不是 E0 重跑所得**。C1–C5 性能 PARTIAL、Root TOOLCHAIN BLOCKED 不改判。E0 没有真实 GPU 行为改动；GPU、Validation、SyncValidation 本轮 **NOT_RUN**，不新增 GPU PASS。Linux、Android、TSan、VTune 本轮 NOT_RUN。

## 2. 源码依赖与删除矩阵

下表均指上述精确基线。完整 `rg` 消费者清单、带行号源码和编译器布局输出随证据归档。

| 类型/机制 | 实际消费者和证据 | 可替代部分、删除前提与处置 |
|---|---|---|
| `execution::CpuScheduler` | [Scheduler.hpp](modules/core/execution/include/lux/engine/core/execution/Scheduler.hpp#L40)、ExecutionRuntime::cpu；Runtime、AssetLoadSender、ScriptAssetAccess | **保留**。它已经是 stdexec Scheduler，是真实宿主工作队列入口，不是本轮要消灭的 `CpuSchedule`。不得换线程池。 |
| `CpuSchedule<Error>`、`cpu_detail::WorkSender` | [WorkSchedule.hpp](modules/core/execution/include/lux/engine/core/execution/WorkSchedule.hpp#L137)；Impl.cpu、DeviceSession::configureUploads、SessionUploads::cpu；CpuFixture、AsyncCost、async_tail | CreateEffect 可直接使用 Impl.scheduler。**E1 仍不能删 Impl.cpu**：initialize 用它配置 Native 上传。全部 Native 及测试消费者迁移后，E4 才能删除适配器和其工厂函数指针。 |
| `CpuSender<Error>` | WorkSchedule.hpp:11，CreateEffect::Operation::cpu_operation_、Transfer::CpuPreparation::operation；async_tail 等夹具 | CreateEffect 改用具体 `connect_result_t`，消除该路径的 any_sender 类型擦除。Native 仍使用它，不能全局删除。不能用另一层 any_sender/any_scheduler 把同样开销加回来。 |
| `CompletionPost<Error>`、`CpuResume<Error>` | [Deferred.hpp](modules/function/render/runtime/pinclude/Deferred.hpp#L90)；CommandBinding Receiver Env；CreateEffect CpuReceiver；Transfer::start、GpuWork::start、CpuFixture | CreateEffect 的手工 post/adopt 可由 `continues_on` 替代。Native 的 prepare→Owner 仍需它。E1 允许两种节点进入**同一条**现有队列，不允许两条完成队列。E3 未通过前保留。 |
| `GetCompletionPost` / `getCompletionPost` | WorkSchedule.hpp:34；[CommandBinding.hpp](modules/function/render/runtime/include/lux/engine/render/runtime/CommandBinding.hpp#L125)、[GpuWork.hpp](modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/GpuWork.hpp#L130)、ServerEffect.cpp:188 | 不是仅服务 CreateEffect 的私有查询。GpuWork 仍通过最终 Receiver 环境取得它；不能提前删除 Env query 或 operationPost。 |
| Render `AsyncWork.hpp` 四个别名及错误常量 | [AsyncWork.hpp](modules/function/render/resource/include/lux/engine/render/resource/AsyncWork.hpp)；Native、Runtime、测试 | 迁完消费者再删别名；错误域的 `kCpuScheduleFailure` / stopped 映射仍需保留准确语义，不因删头而删除可恢复错误。 |
| `CreateEffectSender::Operation`、CpuReceiver、completionPost/adopt | [ServerEffect.cpp](modules/function/render/runtime/src/ServerEffect.cpp#L110)，唯一实例化点经 CommandBinding 连接 | **E1 可替代**。用具体组合表达编译→切回→采用；只保留返回具体 Sender 的私有工厂/成员函数，不保留另一份 CompileState。原 Request、Feature 借用和候选仍有真实寿命，不能凭名称删除其存储。 |
| `PendingOperation` | Deferred.hpp:63；[CommandDispatch.cpp](modules/function/render/runtime/src/CommandDispatch.cpp#L17)、ServerCompletion、CommandBinding 全部异步 Handler | 保留唯一稳定异构 OperationState Owner、路由、stop 和终态交付。详见第 4 节；E1 不能整体替换成 `start_detached` 或 Future。 |
| `CompletionQueue` | Deferred.hpp:27；ServerLoop/ServerCompletion；所有异步终态 | 改节点协议即可作为 RenderScheduler 的唯一 ready 队列。保留短锁和现有 IPC wake；不导入 `run_loop::run()` 作为第二事件循环。 |
| `Transfer::CpuPreparation` | [Transfer.cpp](modules/function/render/native/vulkan/src/transfer/Transfer.cpp#L24)，PREPARE:381 创建，resumeCpu:291 销毁 | 有消除每 Chunk 包装/分配的潜力，但缺少跨模块 Scheduler 和可重用状态的完整证明。当前 **不批准删除**，见第 7 节。 |
| GpuWorkSender / ReadbackSender | Native 专业 Handler、Runtime readBackOutput；[GpuWork.hpp](modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/transfer/GpuWork.hpp#L93)、[Readback.hpp](modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/Readback.hpp#L49) | 已是必要的硬件完成 producer。持有 Source、真实 SubmissionTicket、停止/未知副作用和结果；不能用 `then()` 代替 Vulkan 完成事实。保留。 |
| Core `CompletionWork`、PortSender、Task/TaskScope | EditorUiScene、SceneRuntime、ExecutionRuntime及其公共消费者 | **不是 CompletionPost 的同义重复类型**。服务其他 Owner 的聚合唤醒、Port 或 Task 合同；E4 不得顺便删除。 |

仓内及同级 lux-robotics/lux-communication/lux-dataset 的源文件搜索未发现额外 WorkSchedule 适配器消费者；这不是对未知外部二进制消费者的证明。删除已安装公共头仍需明确版本/API 迁移和独立消费者验收。

### 预计净代码变化

不把原型、文档或历史文件删除算作生产简化。

- 当前 CreateEffectSender 定义约 128 行，包含手写 Operation、CpuReceiver 和 Owner continuation。E1 的组合工厂预计 40–65 行；小 Scheduler（schedule Sender/OpState/Env）约 65–90 行，旧队列节点泛化和唤醒复用另约 10–25 行。**E1 单独可能净增约 0–50 行**；其收益是移除专用状态机、两次实测擦除分配，不能宣称必然净减行数。
- E2 不预设大幅删除：终态存储、通用 Receiver、Outbox、稳定 Pending 和停止资格必须保留。
- 只有 E3 真正迁完，才有资格删除 WorkSchedule 约 165 行、Native CpuPreparation/相关桥、Pending continuation 三字段及其发布分支。合计候选删除约 220–270 行；必须扣除新的跨模块 Scheduler 与 Native 重用接缝，**当前没有可信的最终净值**。
- 若 B 方案最终需要第二次擦除、额外 Owner、每 Chunk Sender 堆分配或泛化 repeat 框架，E3/E4 放弃；保留当前 Native 桥，比换一种包装更简单。

## 3. 唯一 RenderThread Scheduler

建议 **A 方案先行**：Runtime 私有、只借用现有 CompletionQueue。`Scheduler` 是 8B 队列指针值；`schedule()` 的具体 OperationState 内嵌 16B `ReadyNode{next,dispatch}`。新 Scheduler 不拥有线程、队列、Pending 或 IPC。

将队列元素从 PendingOperation 指针推广为 ReadyNode：旧 Native continuation 可使用 Pending 内的节点；新 schedule 使用自身 OpState 节点；最终通用 Receiver 仍发布 Pending 的终态节点。唯一 drain 在 RenderThread 执行节点。短 mutex 只保护连接/摘除；用户函数、析构、Receiver 不在锁内执行。

复用 IpcCore `workEpoch/notifyWork/waitUntil`。保持先取 epoch、后检查队列、最后带 observed epoch 等待的顺序，防止检查与睡眠间丢唤醒。第一次实现 **schedule 总是排队**，即使调用者就是 RenderThread；不增加不可证明的 inline 快路径。Host 继续控制公平预算及真实 GPU 观察，不引入无条件 sleep。

节点在 connect 得到最终稳定地址，start 只一次；调度不会申请节点内存。queue push 完成发布之后不再访问节点；pop 先摘除，dispatch 后不再访问节点。节点与队列寿命不同：队列/IPC wake 对象及模块代码必须晚于所有 producer 尾栈，不能只看队列空就析构。

**容量事实澄清：** 当前 CompletionQueue 是侵入式链，PendingSlots 用可增长的 StableSlotMap，Outbox 用可增长 vector；不能把它们写成“全局固定 128 槽”。128 是本轮原型突发测试量，不是生产全局上限。新方案不增加独立缓存；每个已准入操作在启动前具有稳定通知节点，单节点不得同时二次链接。对多个 Client 的全局业务内存上限，本轮没有新增或证明新的预算。未来不得用一个无界 callback 容器补偿这种差异。

B 方案（Core 的薄 OwnerScheduler）先不落地。只有 Native 出现实际消费者并证明必要，才讨论中立借用节点/函数入口；不能让 Native include Runtime Scheduler，更不能让 Core 引入 IPC 依赖。A→B 是否值得，要和 E3 的状态复用一起衡量。

## 4. PendingOperation 哪些保留

基线 MSVC Release 实测 **272B**，字段偏移见归档 `baseline-layout.log`。

| 字段 | 处置 |
|---|---|
| `response_route_`（32B） | 保留，IPC RequesterId/RequestId 是来源关联，不是 stdexec 调度状态。 |
| `stop`（24B） | 保留；Receiver 环境和 stop_callback 借用它，必须晚于操作状态析构。 |
| `operation_state_` / `destroy_operation_` | 保留，异构、不可移动的 connected OpState 仍需要唯一稳定 Owner。模板组合不等于消除异构存储。 |
| `result_message_`（96B） | E1/E2 先保留。任意外部 Sender 可在 Worker 或同步完成，终态值要安全跨线程交给 Owner。不能从 Worker 直接写 Outbox，也不能凭 CreateEffect 已回 Owner 就删除全局结果槽。 |
| `events`、`publishers` | 保留职责。前者是唯一队列借用；后者保护发布后的 wake/测试钩子尾栈及队列 Owner，不是多余引用计数。 |
| `id` | 保留，StableSlotMap 删除/代际及诊断定位。 |
| `ready_next` | 由通用 ReadyNode 的 link 替代；不是另建队列。dispatch 指针可能增加 8B，不能忽略 padding。 |
| `continuation`、`resume`、`continuation_result` | E1 全保留；Transfer 仍消费它们。只有 E3 完成后删除。按当前布局推算，删三字段并加节点 dispatch 后可能从272B降至224B；**这是布局预测，不是已实施结果**。 |

外部 Sender 的 set_value/error/stopped 均只生成终态并入队；Owner 统一移交 Outbox，再销毁 Pending。同步 Handler 仍原地填写 Outbox，不因“统一 Execution”建立 Pending。连接关闭不能把已经发生的 Native 副作用改为保证 Cancelled；`kOutcomeUnknown` 与 Outbox 的 WOULD_BLOCK 轮转保留。

## 5. CreateEffect 标准组合及生命周期

推荐使用已经存在的 `CpuScheduler`，不必为了字面对应文档强制 `starts_on(just(...))`：

```cpp
schedule(cpu)
    | then([device, request = move(request), features = move(features), limit]() noexcept {
          return compileEffect(*device, request, features, limit);
      })
    | let_error([](EExecutionError error) noexcept {
          return just_error(mapCpuError(error));
      })
    | continues_on(render_scheduler)
    | then(/* Owner: stop check, candidate validation, emplace, Response */);
```

这段是候选结构，不是已实施接口。`mapCpuError` 必须保留真实错误参数；不能把有界 Queue 满/STOPPING变成 terminate。初始 Feature 校验失败应走 `just_error(RenderError)` 分支，仍由唯一终态 Receiver 发布；可用库的 `exec::variant_sender` 表达同一返回类型，不建立新持久 Owner。

`compileEffect` 的失败是 `expected` 的 value-channel 结果，不能假定 `let_error` 会处理它。Owner then 先检查停止，再检查候选；失败返回错误 Response 或展开为既有错误通道，不能调用 effects.emplace。候选通过检查且 Owner 真正采用后才生成 EffectId。计数也只在此更新。

**编译捕获不含 Impl：** 仅 Device、移动后的 Request、稳定 FeatureEntry 借用和 max_config_bytes。采用闭包可借 Impl，但它只在 Owner Scheduler 上运行。Feature StableSlotMap、Device 和模块代码必须活到编译、返回和最终析构；stdexec 不自动保证这些业务借用。不要把整个 Impl 捕进 Worker 来缩短参数列表。

### 时序和销毁证明

1. Owner 取得 Pending 和结果落点，在最终地址 connect，将唯一销毁函数装入 Pending，然后 start。start 同步失败只排终态，不在当前 dispatch 栈上 drain。
2. 宿主 CpuScheduler 在 Worker 前解除自身 stop_callback，移动其运行状态引用到局部，再完成下游；它在下游完成后不读取 ScheduleRequest（Scheduler.hpp:202–218，ExecutionRuntime.cpp:117）。
3. `continues_on` 把 value/error/stopped 存入自身 variant，再 start Owner schedule。安装版本 `__continues_on.hpp:379–406` 在 start 之后不再访问操作状态。
4. Owner schedule 发布节点后的最后一次节点访问在短锁内部。发布者只用提前保存的队列借用执行 wake 和尾栈计数。Owner 可在 Worker 尚未返回时 dispatch；不得在尾栈重新读取 scheduler OpState。
5. Owner 摘节点，局部移动 Receiver，传递 value 或 stopped；候选采用只发生在这里。终态 Receiver 再排 Pending 节点，Outbox 消费发生在独立安全点。保守保留这第二次入队，避免把原异步/同步销毁合同变成重入销毁。
6. stop 先关闭业务准入、请求 Pending 停止；清偿现有 CPU、GPU、ready 节点及发布尾栈，之后才能 reset Responder、销毁 Handler/Device/Instance。宿主仍负责 ExecutionRuntime/模块在全部已接受回调结束前有效。stop token 本身不是 join 或 GPU cancel。

基线 Pending 没有自动模块 CodePin 字段；当前依靠宿主模块寿命合同。stdexec 组合也不会自动钉住 DLL。原型未测试 DLL 卸载，不新增这种资格。后续不能删除已有宿主/插件代码保活约束。

## 6. 隔离原型、编译与成本

原型：`E:/RenderV2Rebuild/E0/prototype/{main.cpp,shape.cpp,CMakeLists.txt}`。没有另外创建线程池；使用真实 `ExecutionRuntime::cpu()` 两个 Worker和既有 IPC wake。侵入式队列只在隔离程序内复现现有短锁 FIFO，并增加节点 dispatch；它没有接进生产 Server。

运行的 compile/adopt 使用 vector 请求和数值候选，刻意不伪装成真实 Graph 编译。`shape.cpp` 另外包含原样ServerEffect.cpp，让候选CPU闭包调用真实compileEffect，并使用真实CreateEffectRequest、FeatureEntry借用、RenderEffect、Response类型做编译期布局探针；Owner闭包只是类型形状，**此探针不链接或运行GPU代码，不构成真实Effect采用资格**。

### 原型结果

| 检查 | 结果及范围 |
|---|---|
| Release、匹配 Debug SDK、匹配 ASan SDK | 原型 `all -j 4 -- -k 0`、第二轮 no-work、CTest各1/1；ASan无诊断。不是整仓/生产的新资格。 |
| C++20 / no RTTI | MSVC `/GR-`，`stdexec::scheduler` concept及connect编译通过。 |
| 禁异常可行性 | 仅隔离 TU `/EHs-c- /D_HAS_EXCEPTIONS=0` 编译运行通过，stdexec异常分支确实关闭。Core/IPC仍是原有编译选项的DLL；不声称整库无异常编译通过，不全局修改工程选项。 |
| 同步 upstream、调度拒绝、业务失败、停止 | inline Scheduler同步完成不内联采用；无效 CpuScheduler的STOPPING和真实容量1队列拒绝均映射Render错误；失败候选不采用；预先停止及编译后/采用前停止不生成成功。 |
| 线程与尾栈 | compile在真实CPU调度路径；adopt/最终Receiver在Owner；节点已发布后冻结Worker尾栈，Owner完成并delete整个组合，再放行Worker，ASan通过。 |
| 多生产者、停服 | 128个真实CPU操作恰好各完成一次；队列容量拒绝后停止已接受排队操作、清偿并join。 |
| 完整生产缺口 | 真实 EffectId发布、Graph失败/重复请求、Native并行编译、GPU关闭、外部DLL Sender/code pin仍需E1生产迁移后回归，未由此原型替代。 |

失败尝试保留：首次编译旧桥对照 Receiver 用自动推导 get_env，在嵌套类型实例化时触发错误stop-token类型；改为显式Env类型。第二次忘记提供真实Host要求的Timer容量，create失败，补齐配置。禁异常版本曾把只适用于预处理判断的stdexec宏用于表达式，已更正。初次Debug用Release SDK的结果不计最终匹配配置资格；随后使用对应Debug SDK重验。均只修改隔离原型。

### 状态与内存：没有“用了 Sender 就更小”

| MSVC x64 Release对象 | 实测大小 |
|---|---:|
| 当前生产 PendingOperation | 272B |
| 当前生产 CreateEffect::Operation（实际CommandBinding Receiver） | 392B |
| 真实业务类型的直接组合布局探针 | 440B |
| 上述组合加初始错误 variant 分支 | 448B |
| 合成同工作量旧桥接Op / starts_on组合Op / 简洁schedule组合Op | 272 / 440 / 352B |
| Render Scheduler值 / ReadyNode | 8 / 16B |
| 当前 CpuSender擦除值 / CpuSchedule | 40 / 16B |

Debug合成Op分别280/448/360B；不能将不同配置布局混比。真实类型探针只冻结了候选数据形状；最终错误/stop封装可能改变448B，必须在E1重新实测。当前实际CreateEffect生产状态由原样ServerEffect.cpp的MSVC布局输出取得，没有修改生产代码来读取private字段。

预热后的合成对照测得：旧桥**每次2个EXE分配、累计请求200B**；两种标准组合均为0。包含connect/start/完成交付/栈Op析构，输入vector准备在计时和计数前。生产CommandBinding外层的`new Operation`、Pending增长、真实compile内部容器、DLL内分配均不在此“0”的承诺内。不能从这个局部实验得出完整CreateEffect零分配，也不能拿392与448的表面大小忽略旧擦除对象的外置存储。

### 调度初测

10个独立进程，每种路径每进程100次预热、1000次正式样本；old/new/lean逐次交替。使用同一CPU Scheduler、同一队列、相同vector求和与Owner采用；不绑核。主计时包括connect、CPU排队、wake/等待、两次Owner事件和析构。下表为各run分位数的中位数，max取所有run最大值，单位µs。

| 路径 | p50 | p95 | p99 | max | 每次Owner事件 |
|---|---:|---:|---:|---:|---:|
| 旧CpuSchedule/手工continuation形状 | 5.65 | 15.80 | 25.85 | 15631.8 | 2 |
| starts_on + continues_on | 5.65 | 15.70 | 23.85 | 14348.4 | 2 |
| schedule + then + continues_on | 5.55 | 15.85 | 26.05 | 15225.3 | 2 |

全部30,000个正式样本保留。长尾未归因；计时包含Host/OS调度及最多1ms的请求等待deadline，不能叫纯IPC队列延迟。100ns左右的计时量化、固定old/new/lean次序及机器状态限制了这些初测。p95接近不证明等价，p99也没有一致改善；**性能结论仍PARTIAL**。真实Graph/Vulkan工作量完全不在此对照中。

本轮可以证明局部擦除分配减少、专用continuation可替代；**不能证明CPU→Owner交接或最终事件次数减少**。若E2要把Owner已知终态直接送Outbox，必须另证start/回调尾栈及通用外部Sender仍安全，不能偷删终态安全点来做漂亮数字。

## 7. Transfer、GpuWork和Readback裁决

Transfer 当前按 `PREPARE → CPU_PENDING → COPY/COPY_PENDING → ACQUIRE/FINAL_PENDING` 驱动，每次PREPARE用 `make_unique<CpuPreparation>`，Owner resumeCpu reset旧对象，再按真实GPU完成推进。该状态机还有Staging页、格式行拷贝、部分提交、Queue容量、QueryPool及Source借用，不是仅一串then。

`CpuPreparation` 的 Receiver使用empty_env；取消由外层GpuWork/Transfer及真实已接受工作共同控制。不能机械把外层stop token传播进去并在Copy未安全结束时销毁Source或物理Backing。GpuWork::finish在submitted且stop/deviceLost时保留Unknown语义，publish只能在真实完成后执行。

E3有两个不能越过的缺口：

1. Runtime私有Scheduler不能注入Native具体OpState。若为了传入它再使用any_scheduler或新增抽象Port，可能重新引入本轮刚消除的分配/间接层。需要先证明中立Core薄借用接缝是实际必需且没有第二Owner/队列。
2. OperationState不可移动且start一次。每Chunk在optional内销毁/重建并非天然错误，但必须逐一证明旧sender、stop callback和producer尾栈最后一次访问已结束，或证明最后访问早于发布且尾栈只持外部局部值；不能只凭“Owner收到了通知”假定整条链可复用。E0的单次链尾栈实验**没有证明GPU Chunk循环里的全部复用交错**。

因此当前 E3 **NO-GO（删除/整体替代）**。建议保留唯一Transfer和当前CpuPreparation，后续如单独验证每Transfer一次嵌入/分配、每Chunk重新构造具体OpState，应使用现有三Queue拓扑、部分提交、停止、Source计数和ASan夹具。不得为E0补齐这个结论而开始Native改造。

E5的GpuWorkSender/ReadbackSender目前已经是最接近真实完成生产者的位置。保留其Ticket、回调摘链、LastUse、SourceOwner、Readback订阅和Unknown处理；不要再套一层continues_on强制把已经在RenderThread的GPU完成排一次队。没有证据显示将其改成纯标准组合能净减少复杂度，建议不改。

## 8. E1–E5推进意见

| 阶段 | E0意见 | 必须满足的条件 |
|---|---|---|
| E1：唯一Owner Scheduler + CreateEffect | **GO，有界** | A方案、具体CpuScheduler、单队列；删除专用Operation/CpuReceiver/adopt，不保留并行Owner。真实编译采用、错误映射、初始失败、停止、重复请求、尾栈、GPU并行及SDK回归后才可生产PASS。初测不作为性能PASS。 |
| E2：通用终态/Pending | **PARTIAL** | 可统一节点和dispatch；保留路由、stop、结果、异构操作Owner和publisher保护。不要为了减字段改变Outbox背压。Native continuation仍在时不验收其删除。 |
| E3：Transfer CPU preparation | **NO-GO，当前证据不足** | 先有中立调度接缝及每Chunk稳定重用证明；若新增擦除/分配/框架超过被删内容则取消本候选，保留现状。 |
| E4：删除WorkSchedule适配器 | **NO-GO（依赖未满足）** | E3完成及所有生产/测试/安装消费者迁移后重审；不删除CpuScheduler、CompletionWork等真正公共能力。 |
| E5：GPU/Readback完成审计 | **审计可GO；重写NO-GO** | 维持原真实producer。只有具体重复逻辑证据才做小改，禁止更换Submission/退休状态机。 |

E0结束，不自动执行以上阶段。最合理的下一次授权是只做E1及必要的E2节点接缝，保留Native桥；不把E3–E5捆绑成必须完成的“统一框架”。

## 9. 复现、保护与归档

执行入口 `E:/RenderV2Rebuild/E0/run.ps1`：

```powershell
./run.ps1                         # RelWithDebInfo
./run.ps1 -Config Debug -Mode d   # 匹配Debug SDK
./run.ps1 -Mode asan              # /fsanitize=address /Zi
./run.ps1 -Mode noeh              # 仅原型禁异常可行性
```

每次配置后执行 `cmake --build <build> --target all -j 4 -- -k 0` 两遍，再 `ctest --test-dir <build> --output-on-failure -V -j 1`，测试timeout45秒。ASAN_OPTIONS为`alloc_dealloc_mismatch=1`。性能运行和构建串行；原型、命令、日志、失败尝试、原始CSV、逐run统计、编译器布局与来源哈希全部外置保存。

Legacy719个冻结blob一致；两处主工作树用户文件及V12工作树的RenderServer.hpp三个SHA保持。仅允许新增本文件；无生产/测试/CMake diff，旧报告未改。没有修改公共头，因此不写入三个共享SDK前缀。

原始证据：[E0-stdexec-evaluation.zip](E:/RenderV2Rebuild/archives/5288a759dd5fa3565fe7eedeb208d26a1d57d25e/E0-stdexec-evaluation.zip)，SHA-256 `7ee90d922d6b990b033f5ca7ad5f060a90cb32ac2ccc03a366cd1131b6c34bcf`。包含104份文件及逐文件SHA256_INDEX.json，ZIP CRC和全部条目哈希已复核。归档包含原型源、构建入口、依赖/SDK哈希、三轮初测与最终样本、每次失败记录和精确源码引用，可复制归档重新检查；运行需按脚本配置本地依赖位置。

一次tracked snapshot检查因正在编写的未跟踪E0_REVIEW.md被正确拒绝；随后对相同基线的干净工作树检查通过。没有忽略或隐藏这个文件来伪造生产资格。报告提交后再检查本评估树干净；本轮报告的Parent必须为上述精确基线，提交自身完整SHA从交付信息/Git获取。

报告只提供E0资格，不覆盖任何历史判定。没有生产Implementation I，也不虚构一份E1–E5 Verification V。
