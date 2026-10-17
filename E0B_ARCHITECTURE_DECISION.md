# RenderServer stdexec：E0-B 架构决策

**建议先实施 P1；P1 不再受 P3 阻断。** 在保留现有 Native 完成桥、CPU 准备对象及其销毁点的条件下，两个真实生产 TU 的隔离副本已能使用具体 `CpuScheduler` 编译。中立 OwnerScheduler 的标准查询与借用接缝也可行，但这尚不等于已经证明可以删除 Native 完成桥。本轮没有修改或运行新的生产路径。

本报告修订 E0 的后续建议，不改写 `E0_REVIEW.md`、既有 I/V 或历史资格。只执行附件 §8 的 P0 实验和 P1 可行性验证；没有实施 P1–P5。

## 1. 基线与证据范围

- 当前远端及本轮起点：`421b477d1e68c9d0b5c8f81c4d8eda38ac8eebf4`，分支 `codex/render-v2-rebuild`。该提交相对 `5288a759dd5fa3565fe7eedeb208d26a1d57d25e` 仅增加 E0 报告，生产代码未变。
- 隔离评估树：`E:/RenderV2Rebuild/E0B/source`，评估分支 `codex/render-stdexec-e0b`。唯一仓内变更为本文件；可编译原型、四份真实源码副本、日志和样本均在源码树外。
- 本轮任务文档 SHA-256：`b66bc509690d489668cdb481bd336cad341148ddb8242030d53f385559c262d9`。
- lux-cxx：`bf779515a120350c7c5412c366eea73afdf59ccf`；toolset：`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，依赖树干净。
- MSVC 19.44.35226、Windows x64、i9-13900H（14核/20逻辑处理器）；stdexec 0.10.0，本机 port REF `7d704575019a1405b2067c5e47265245c55ca66e`。安装头另有完整 SHA-256 清单。
- 使用 E0 已核对的 Core Execution/IPC SDK：Release 为 C1–C5 consumer-sdk，Debug/ASan 为 E0 私有 SDK。本轮重新核对 34 个相关公开头与精确基线一致，记录 DLL 哈希。没有从脏主工作树补头，没有修改三个公共安装前缀。

Legacy 719 blobs、主工作树 `.vscode/settings.json`/`Graph.hpp`、V12 工作树用户 `RenderServer.hpp` 均校验不变。旧报告、测试和生产 CMake 未改。旧 C1–C5 Release 146/146、Debug 145/146（`graph.edit` 失败）、ASan 146/146 仍仅是历史结果，**本轮没有重跑这组生产测试**。性能 PARTIAL、Root TOOLCHAIN BLOCKED 不改判。

## 2. 决策与拆分路线

| 候选 | 本轮结论 | 下一步边界 |
|---|---|---|
| P0：A/B 标准调度接缝 | **GO（隔离可行性）**；中立 B 的生产采用仍为 **PARTIAL** | B 可从独立 Native 形状的 TU 使用，无 Runtime/IPC/Vulkan 依赖，无额外队列或分配。但尚未迁移真实 GpuWork/Transfer，不能声称已净删 Native 桥。无需为实施 P1 先新增 B。 |
| P1：去 CPU 类型擦除 | **GO，优先候选** | CreateEffect 和 Transfer 同次完成具体 CPU Sender 迁移；保留 CompletionPost、CpuResume、Pending continuation 和每 Chunk CpuPreparation。需正式授权后的生产回归/安装验收，不能把本报告当生产 PASS。 |
| P2：CreateEffect 标准组合 | **有条件 GO** | E0 的可行性继续有效，且可独立于 P3。先取得 Owner，CPU 只编译，Owner 才采用。P1 已有明确删除收益，不必为了推进 P1 同时引入 Scheduler。 |
| P3：Native CPU→Owner 桥替换 | **PARTIAL** | 需要真实 Native 消费者、尾栈/停止/部分提交证明及净删除清单。直接 `then(resumeCpu)` 自删状态、或对全部 GPU 终态追加可停止的 `continues_on`：**NO-GO**。 |
| P4/P5 | **未实施** | Chunk 状态复用独立评估；只有实际消费者清零才删旧桥/Pending continuation。无需与 P1 绑定。 |

因此撤回 E0 中“必须等 E3 才能推进 CPU 适配器全局删除”的路线限制。需要等待的是**所有 CPU 适配器消费者迁完**，不是等待 Native 回传协议或 Chunk 状态复用完成。

不建议现在同时把 A 和 B 放进生产。如果后续真实 Native 迁移能净删桥接，B 是合理的共享借用协议；若不能，采用 Runtime 私有 A 服务 CreateEffect 并保留 Native 桥。两种选择均不阻碍 P1。

## 3. 实际消费者与删除矩阵

下列行号绑定生产基线 `5288a759dd5fa3565fe7eedeb208d26a1d57d25e`。归档包含完整 `rg` 输出和带行号源码。

| 源码/消费者 | P1 处置 | P3/P5 才可能处理的部分 |
|---|---|---|
| `core/execution/.../WorkSchedule.hpp:11,43,137` | 删除 `CpuSender<Error>`、`cpu_detail::WorkSender`、`CpuSchedule<Error>` 及 any_sender 依赖 | 保留 `CpuResume`、`CompletionPost`、`GetCompletionPost`；不能整头删除 |
| `runtime/pinclude/ServerImpl.hpp:96–97`；`src/RenderServer.cpp:14–26` | 保留已有 `scheduler`，删重复 `cpu` 字段及 bind/map 工厂路径；向 Native 传具体 scheduler | Pending、CommandRegistry、Outbox 与 GPU 状态不变 |
| `runtime/src/ServerEffect.cpp:110–233` | 私有 Operation 与 CpuReceiver 继续存在，仅以 `schedule/then` 连接具体 CPU Operation，Receiver 将 `EExecutionError` 映射为原 RenderError | P2 才删除专用 CpuReceiver/adopt 机制；候选和 Request 所有权仍必须存在 |
| `native/vulkan/.../DeviceSession.hpp:58`、`pinclude/SessionUploads.hpp:20,25` | `configureUploads`/UploadState 使用具体 `execution::CpuScheduler` | 不移动 Device/Transfer Owner，不引入 Runtime 依赖 |
| `native/vulkan/src/transfer/Transfer.cpp:24–54,288–308,381` | CpuPreparation 内连接具体 Sender；保留 make_unique、CompletionPost、resumeCpu 的 reset 时机和后续状态推进 | P3 只评估交接；P4 才评估本体分配/复用 |
| `resource/.../AsyncWork.hpp:11–14` | 删 CpuSender/CpuSchedule 别名，保留 CPU 错误与桥别名 | 全部桥消费者清零后再删剩余别名 |
| `runtime/.../CommandBinding.hpp:125`、`CommandDispatch.cpp:39`、`Deferred.hpp:77–112` | **不变** | Native 真正迁完后才考虑以标准查询替换自定义查询、删 continuation/resume/result 三字段 |
| `native/.../GpuWork.hpp:130`、`GpuWork.cpp:10`、`Transfer.hpp:36,76` | **不变**；仍是 CompletionPost 的真实消费者 | B 实验不代表这几条真实路径已经迁移 |
| `core/execution/test/WorkScheduleTest.cpp` | 迁移 CPU 部分断言，保留独立桥行为断言 | 不可因为公共类型删除而丢弃对应行为测试 |
| `resource/test/CpuFixture.hpp`；Runtime `ServerFixture`、`Offscreen`、`TextureProgress`、`AsyncCost`、`async_tail` | 迁移旧擦除类型和测试注入。拒绝用真实有界 CPU 队列/停止；同步第三方 Sender 仍单独覆盖 | 不为兼容故障夹具保留生产擦除；不删同步、任意线程完成、模块尾栈行为 |
| `AssetLoadSender`、`ScriptAssetAccess` | 已使用具体 CpuScheduler，无需迁移 | 不改 Core 的 Task/PortSender/CompletionWork，它们具有其他真实消费者 |

P1 不需要新增长期类型。局部 `Compile`/`Prepare` 是有状态可调用值，替代原 work 函数指针及 `void*`；具体 Operation 由 stdexec 生成。预计生产代码净减少约 **80–120 行**，主体来自 WorkSchedule 的擦除实现；这是实施估算，非已合并 diff。测试迁移不计作生产简化。

P0 的 A 与 B 都需要一个 schedule Sender/内嵌 Operation/就绪节点协议。A 借 Queue 指针；B 借 `{context, enqueue}`。B 放入 Core 前必须有真实 Native 消费者，不能为“统一”再保留 A 的平行实现。当前 B 原型 69 行左右不代表最终净收益：尚未扣算真实桥迁移、安装与尾栈协议。**本轮没有证明 P3/P5 最终净删除量。**

## 4. P1：真实源码的隔离编译

`compile_p1.py` 从精确源码读入并生成四个外部副本：Transfer.cpp、SessionUploads.hpp、DeviceSession.hpp、ServerEffect.cpp。使用原生产 compile_commands 的 include/define/C++20 选项，分别编译原件与候选，并取得 MSVC class layout。没有链接替换生产 DLL。

候选仅替换 CPU 连接类型与错误映射：

```cpp
stdexec::then(stdexec::schedule(cpu), Prepare{&transfer})
```

CreateEffect 使用已有 `Impl::scheduler`；继续由原 CpuReceiver 投递 CompletionPost，再由原 adopt 在 Owner 修改 Effect 表。Native 使用原 Receiver 的 `empty_env`，没有借此次实验增加新的 stop-token 语义。普通 lambda 均 noexcept；编译器验证连接通道为 value / EExecutionError / stopped。

脚本断言 **Transfer::resumeCpu 至文件末尾逐字不变**、CreateEffect 的 adopt 函数逐字不变。包括 reset、GPU COPY/ACQUIRE、SubmissionTicket、Source 生命周期及候选采用次序；可审阅 `p1-overlay.diff`。

边界差异显式保留：具体 CpuScheduler 没有 `operator bool`。隔离配置入口用与默认 Scheduler 比较识别空句柄；旧 `CpuSchedule::operator bool` 只检验工厂是否存在，并不证明 Host 仍接受工作。正式 P1 需固定默认/已停止 Scheduler 的入口错误合同；运行时拒绝仍映射准确的 EExecutionError，不能把一次冷态检查当持续可用证明。

| MSVC 实际布局 | 当前生产 | P1 副本 | 说明 |
|---|---:|---:|---|
| CreateEffect 实例化 Operation | 392 B | **360 B** | 原完成桥和采用函数保留 |
| Transfer::CpuPreparation | 112 B | **80 B** | 本体仍每 Chunk 分配，不承诺零分配 |
| DeviceSession::UploadState | 176 B | **184 B** | 具体 Scheduler 值比原双指针适配器大 8 B |
| PendingOperation | 272 B | 不改 | P1 无需修改其状态或异构存储 |

这是 P1 与 P2 的重要区别：E0 标准 CreateEffect 全组合的 440–448 B 是另一候选；不能拿它代替本次保留桥的 360 B，也不能把局部内存缩减写成总 GPU 内存缩减。

## 5. P0：中立 Scheduler、标准环境与两个反例

### A/B 接缝

原型 A 是 8 B 的 Queue 借用。B 为 16 B 的 context/function 借用；二者的 schedule Operation 内嵌同一 16 B ReadyNode，都进入**同一条**原 CompletionQueue 形状的短锁侵入队列，使用实际 IPC `notifyWork/workEpoch/waitUntil`。没有新执行线程、就绪队列、业务 Owner 或引用计数。

B 的 enqueue 从独立 TU 的 `opaqueOwner` 返回，没有 LTO，保留函数指针间接调用。`NativeProbe.cpp` 仅链接 Core Execution；Ninja 记录的 245 项依赖不包含 Runtime、Render、IPC、Vulkan。它验证的是独立模块可消费的 Native 形状 Sender，**不是执行了真实 GpuWork/Readback/GPU**。完整程序为使用真实宿主队列与 IPC 唤醒，另外链接 Core Execution/IPC。

编译与运行证明：Scheduler concept、`get_completion_scheduler<set_value_t>`、Receiver `get_scheduler`、then/let_value 前向、value/error/stopped 的 Owner 交付均可成立。CreateEffect 形状的 lean 组合由 A 的 352 B 增至 B 的 **360 B**；没有新增原型局部分配。

### 反例一：get_scheduler 不是永久 Owner 标签

在 Native 入口从原 Receiver 环境取得 B 可行。但实测：

```text
starts_on(cpu, read_env(get_scheduler)) → 读到 CpuScheduler
```

标准查询表达当前执行上下文；它能被组合器覆盖，不是自定义 `GetCompletionPost` 的无条件同义替换。建议在原 Owner 启动 Native Sender 时取出明确的 Owner 借用，再显式捕获到返回链。若任意外部组合先将 Native start 移至 CPU，必须拒绝这种线程使用或另有明确的 Owner 合同，不能假设返回值仍是 OwnerScheduler。

### 反例二：错误不能被停止信号覆盖

同一已停止的 Receiver，直接接收 `just_error(CommandFailure{outcome_unknown=true})` 保持 `kOutcomeUnknown`；在其后无条件追加本原型可停止的 `continues_on(B)`，实际转为 `set_stopped`。这是**预期触发的错误设计反例**，不是生产回归或 GPU 测试。

所以 Native 最终终态不得机械追加 Owner hop。已在 Owner 完成的 GPU Sender 应继续原通路；若后续 P3 需要强制回 Owner，应另证强制清偿交接不会屏蔽已发生副作用的错误。不能仅凭 schedule concept 通过就认为满足业务完成合同。

## 6. 生命周期证明与必须保留的状态

现有 P1 的安全交接可保持如下：

```text
Owner: 创建稳定 CpuPreparation → start
CPU:   prepare → terminal Receiver → post(Pending/Host 节点)
Owner: pop 并摘节点 → resumeCpu → reset CpuPreparation → 继续 Transfer
CPU:   发布后的 wake/尾栈结束（此时不再访问已发布节点/Operation）
关闭:  队列排空 + publishers 清偿 + Host 工作停止/join，之后才能释放队列和代码
```

原型冻结 Worker 的发布尾栈，让 Owner 完成并销毁旧/新 Preparation；两种模式均销毁一次、拷贝数据正确，MSVC ASan 无诊断。真实源码副本没有移动 reset 点。阶段错误也不混用：CPU 调度错误、prepare 的 expected 错误、GPU 已接纳后的 Unknown 是不同事实。

对附件“OperationState 不能在自己的完成调用栈中销毁”需要作精确限定：**终态 Receiver 可以销毁状态，前提是所有上游实现之后都不再访问它**。这也是标准连接状态通常支持的终态用法。原型在实际 pinned stdexec 和 CpuScheduler 上验证了这种尾部交接。不能由“栈还在”单独推出 UAF，也不能由某一终态场景安全推出任意 adaptor 内部都可自删。

`then(resumeCpu)` 的问题不同：lambda 删除自己的组合状态后，`then` 还需要该状态中的下游 Receiver 发出完成。源码证据是 pinned stdexec `__detail/__then.hpp` 的 `__set_value_from(state.__rcvr_, state.__fun_, ...)`。因此不能把原 terminal post 直接搬进会自删状态的 then。P3 若需要再加一份 Resume/退休状态或重复 Owner hop 才安全，应比较净复杂度，允许保留原桥。

Pending 的必要职责不因本轮改变：唯一异构不可移动 Operation 所有权、IPC 来源路由、stop source、任意线程终态的 `result_message_`、发布尾栈保护。P1 不能删除 continuation 三字段；P3 消费者全部迁完后才考虑。ResponseOutbox 仍只能在 Owner 操作。CPU Scheduler 借用、队列、enqueue 回调代码及 Code Pin 必须覆盖最后一个回调尾栈，不能仅以队列空判定可以卸载。

本轮未重新验证 DLL 卸载、真实 Source GPU 保活、DeviceLost 或部分 Submit；这些仍是生产迁移的门禁，不能由 CPU ASan 实验代替。

## 7. 实测成本与性能资格

所有原始样本保留。Release、同机，10 个独立进程，每个模式预热 100 次、采样 1,000 次；同一进程四模式轮换先后顺序。使用实际两 Worker CpuScheduler、同一 Owner queue/IPC wake。输入 vector/4096 B 准备在计时外；计时包含 connect/start、工作、两次 Owner 事件、终态消费、发布尾栈清偿和局部 Operation 析构。

A/B 使用相同合成 CreateEffect 工作；prep_old/prep_direct 使用相同 4096 B memcpy。两组间不作加速比较。这里没有 Shader 编译、Vulkan、IPC 请求路由或完整 RenderClient；不是生产端到端结果。等待使用带 epoch 的最多 1 ms 等待，没有无条件 sleep；OS 唤醒和调度包含在测量中，不能称为纯队列延迟。

| 路径 | 每 run p50 中位数 | p95 中位数 | p99 中位数 | 全部样本最大值 |
|---|---:|---:|---:|---:|
| A：Runtime 私有 Owner | 18.10 µs | 26.75 µs | 63.45 µs | 15,638.5 µs |
| B：中立借用 Owner | 17.80 µs | 28.00 µs | 57.25 µs | 14,993.2 µs |
| 保留桥，擦除 CPU Sender | 18.85 µs | 28.30 µs | 49.60 µs | 15,327.8 µs |
| 保留桥，具体 CPU Sender | 18.65 µs | 28.65 µs | 64.75 µs | 14,637.8 µs |

A→B 的逐 run p95 配对变化中位数 **+1.70%**，范围 **−3.08% 到 +10.34%**；p99 更不稳定。不能从总体分位数相近宣布 B 无退化，也不能把个别尾部差异归因于一次间接调用。初轮 pilot 全部保留；其统计不同，进一步说明此处未建立性能等价性。约 15 ms 离群未归因，不删除。

另测仅 Owner 本线程 schedule→enqueue→drain→终态路径，每个样本批量 32 次，每模式 1,000 样本、10 进程：A/B 每 run p50/p95/p99 的中位数均为 **131.25 / 162.50 / 184.375 ns**。包含两次入队/通知和两次出队、状态构造析构，不是单独函数指针成本。没有 CPU 线程切换；不得用它解释真实跨线程尾延迟或宣称间接调用免费。

| 原型局部计数 | A/B | prep_old | prep_direct |
|---|---:|---:|---:|
| EXE new 次数/操作 | 0 / 0 | **3** | **1** |
| EXE 累计分配字节/操作 | 0 / 0 | **320 B** | **88 B** |
| Owner 事件/操作 | 2 / 2 | 2 | 2 |

准备原型比真实 CpuPreparation 多 8 B 的析构计数 Host 指针，所以其 120→88 B 不是生产的 112→80 B。统计覆盖 EXE 的 operator new，不覆盖 DLL/Driver 分配。P1 删除两次局部擦除分配有直接证据，但保留一次本体分配；没有宣称每 Chunk 零分配、整个请求零分配或 GPU 性能改善。两个 Owner 事件也没有减少。

**性能资格仍 PARTIAL。** 本轮没有 VTune、CPU 固定核、Server idle、四 View、真实 Chunk/GPU 时间或 GPU timestamp 的新测量。正式 P1 的收益判断优先看确定的类型/分配删除；真实时延需生产等工作量对照。

## 8. 验证结果与复现

| 验证 | 本轮结果 |
|---|---|
| 原型 RelWithDebInfo、Debug、MSVC ASan | 各 all 构建、第二轮 no-work、CTest 1/1；ASan 无错误 |
| 原型严格 no-exceptions 配置 | `/EHs-c- /D_HAS_EXCEPTIONS=0`、C++20、`/GR-`，构建与 CTest 通过；仅用于外部原型，不更改项目异常策略 |
| A/B 标准查询、Owner 亲和性、成功/失败/停止 | 通过隔离行为检查；两个语义反例按预期触发 |
| 发布尾栈、同步重入、0/1/16/129 突发 | 通过；事件计数一致；129 不是生产容量上限 |
| P1 默认 Scheduler 拒绝、真实容量 1 饱和、Host stopping | 旧/新准备路径均精确一次清偿，失败不运行 work；保留原桥/reset |
| 真实源码原件/候选编译 | Transfer、ServerEffect 两 TU 各自通过；布局输出归档，未链接运行候选 Native |
| Native 形状 TU 依赖闭包 | 无 Runtime/Render/IPC/Vulkan include；不等于完整 Native 库移植完成 |
| 基线 tracked snapshot、保护校验 | 通过；唯一报告白名单；719 blobs/用户修改/历史文件不变 |
| 全量生产、GPU/Validation/SyncValidation、独立 SDK relocation、外部 TestBuffer、多 Client/Outbox | **NOT_RUN**（E0-B 未改生产）；既有证据不冒充本轮结果 |
| Linux、Android、TSan、VTune | **NOT_RUN** |

主要复现入口（源码树外）：

```powershell
powershell -NoProfile -File E:/RenderV2Rebuild/E0B/run.ps1
powershell -NoProfile -File E:/RenderV2Rebuild/E0B/run.ps1 -Config Debug -Mode d
powershell -NoProfile -File E:/RenderV2Rebuild/E0B/run.ps1 -Mode asan
powershell -NoProfile -File E:/RenderV2Rebuild/E0B/run.ps1 -Mode noeh
# 各配置内部执行 all -j 4 -- -k 0 两次，随后 ctest -V -j 1；ASAN_OPTIONS=alloc_dealloc_mismatch=1
# 在 MSVC x64 开发环境：
py -3 E:/RenderV2Rebuild/E0B/compile_p1.py
E:/RenderV2Rebuild/E0B/build/r/e0b.exe
E:/RenderV2Rebuild/E0B/build/r/e0b.exe --owner-cost
py -3 E:/RenderV2Rebuild/E0B/collect.py
```

环境路径、编译命令、原始 CSV、配对统计、全部失败尝试均归档。初期原型出现两个编译错误：新增错误重载的花括号歧义、环境查询测试漏接 EExecutionError；已在原型修正。脚本初次 Windows 路径替换也失败，日志保留。未隐瞒为生产失败或删除这些尝试。编译保留 `stdexec::empty_env` 弃用警告；候选为保持现有 Native receiver 环境语义，未借机修改生产代码。

证据包：`E:/RenderV2Rebuild/archives/421b477d1e68c9d0b5c8f81c4d8eda38ac8eebf4/E0B-architecture-evaluation.zip`。

证据 SHA-256：`95c97fdc614e4dcbb53fe221ca10879ab74290b510934e8b11f1c230d91b22c6`。包内 `SHA256_INDEX.json` 对每个文件记录哈希、大小；包含 `prototype/`、`p1-overlay/`、复现脚本、`evidence/`，解压后根据实际 SDK 位置调整路径即可重跑。该包是评估原型，不是生产安装包。

本轮结束于评估。推荐下一次有界生产任务只做 P1，完成其实际消费者和回归；P2 可另行推进，P3 继续以真实 Native 生命周期及净删除收益为门槛。没有提前删除 Pending、CompletionPost 或 Vulkan 固有状态，也没有开始 E1–E5/P1–P5 实施。
