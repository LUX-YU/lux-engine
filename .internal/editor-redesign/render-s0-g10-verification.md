# S0 / G10 独立验收 V

结论：**S0 PASS；G10 PASS（本指令限定的 Windows Vulkan 路径）**。
根 TOOLCHAIN 产品闭包仍为 **BLOCKED**；IPC R2 历史性能 **PARTIAL** 不变。
G11–G13 **NOT_STARTED**。从本轮正确性门禁看可提交 G11 入场审阅，未取得后续施工授权。

## 精确输入、提交链与证据

- 起点 / 最终推送前预期远端：`be9d7af3d4378a1115d9373bc4e73bc5d7bb68c2`。
- S0-I：`9b0bc12723fb63f781a413755d0ee4e8eb55fed6`，Parent 为上述起点。
- G10-I / 本 V 唯一生产输入：`528c2c1f399df29343b9fe81d78be5160decd852`，Parent 为 S0-I。
- 本 V Parent 为 G10-I；V 自身完整 SHA 在提交后的交付信息中给出，避免自引用。
- 目标仅 `codex/render-v2-rebuild`，正常 fast-forward；没有 main 合并或 force-push。
- 规范 SHA-256：`44b6c2c258c0a64a081c718482b50da35037a6cc0803a5466770cfec91b50e38`。
- lux-cxx：`bf779515a120350c7c5412c366eea73afdf59ccf`；toolset：
  `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`，最终检查均干净、无 submodule。

实施 worktree：`E:/RenderV2Rebuild/S0G10/source`。
独立 clone：`E:/RenderV2Rebuild/S0G10/verification/528c2c1f399df29343b9fe81d78be5160decd852/source`。
clone 使用 `--no-hardlinks --no-checkout`、detach 精确 I，无 alternates。先执行 tracked snapshot，再配置全新构建树。
验收结束后 source 仍干净，未修源码、未补本地隐藏头。V 仅增加本报告。

不可变证据索引（541 项，每项有字节数和 SHA-256）：
`E:/RenderV2Rebuild/S0G10/archive/528c2c1f399df29343b9fe81d78be5160decd852/INDEX.json`

索引 SHA-256：`d8a6e820f0c342ce8479816810a6488a232b253d92afc30726788fd597095252`。
索引包含实施/独立验收日志、失败尝试、原始 CSV、配置/编译命令/安装清单、保护快照、完整 diff、执行脚本及规范副本。
`verification/performance-summary.json` 可由归档的 `runners/analyze_samples.py` 从所有原始行重算，未删异常样本。
S0 独立实施归档仍保留在 `archive/9b0bc12723fb63f781a413755d0ee4e8eb55fed6/INDEX.json`，
其 SHA-256 为 `e5beb6512b9e99e01014fa34f7741935bf731bfeaf8a9e0c914d9b95a129eab5`。
交付/push 产生的后续日志另存 `E:/RenderV2Rebuild/S0G10/delivery/`，不回写上述索引。

## 构建、运行和安装

环境：Windows 11 / MSVC 19.44.35226.0 / CMake 3.31.10 / Ninja；NVIDIA GeForce RTX 4070 Laptop GPU。
真实查询 API=4211029（1.4.341）、driver=2559967232（NVIDIA 编码 610.88.0.0）；SDK 1.3.280.0。
Foundation 输出明确记录 Validation 和 Synchronization Validation 均 enabled。

| 矩阵 | all / 第二轮 | CTest | 耗时 | 资格 |
|---|---|---|---:|---|
| S0 内部门禁 RelWithDebInfo / Debug / ASan | 通过 / no work | 各 61/61 | 49.98 / 51.71 / 63.70 s | PASS |
| G10 内部 RelWithDebInfo / Debug / ASan | 通过 / no work | 各 62/62 | 75.57 / 67.96 / 88.47 s | PASS |
| 独立 V RelWithDebInfo | 127 构建步骤通过 / no work | 62/62 | 86.68 s | PASS |
| 独立 V Debug | 127 构建步骤通过 / no work | 62/62 | 91.26 s | PASS |
| 独立 V MSVC ASan | 127 构建步骤通过 / no work | 62/62 | 92.85 s | PASS |
| 每公共头独立 C++20 / 无 RTTI | 三配置构建包含 | 编译命令检查通过 | — | PASS |
| 新前缀、移动前缀、Native/Runtime 等 installed consumers | 三矩阵原有安装用例 | 全部运行 | — | PASS |
| 额外纯 Client、Vulkan 查找明确禁用 | all 两轮 / 第二轮 no work | 1/1 | 见日志 | PASS |
| 根 TOOLCHAIN | 实际 configure 返回 1 | 无法进入根 all | — | BLOCKED |

S0 后追加实际 64 MiB Fill / 零截止时间 Fence 测试，三个配置均观察到真实 Timeout，重建与受影响 Native 用例通过；
最终 V 重新运行了全部测试。`matrix-integrity.json` 比较名称集合，旧 61 项全部存在，唯一新增为
`render.runtime.gpu_ready`，没有用重命名、删除测试凑数量。三配置 source/include/link/install 检查均通过。

所有构建与实机测试串行。精确脚本已归档，核心命令如下（VROOT 为上面的 verification/I 目录）：

```powershell
git clone --no-hardlinks --no-checkout E:/RenderV2Rebuild/S0G10/source <VROOT>/source
git -C <VROOT>/source checkout --detach 528c2c1f399df29343b9fe81d78be5160decd852
cmake -DLUX_SOURCE_DIR=<VROOT>/source -P <VROOT>/source/cmake/ValidateTrackedSnapshot.cmake
cmake -S <VROOT>/source/cmake/render-v2-bootstrap -B <VROOT>/release -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_CXX_FLAGS=/EHsc `
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <VROOT>/release --target all -j 4 -- -k 0
cmake --build <VROOT>/release --target all -j 4 -- -k 0
ctest --test-dir <VROOT>/release --output-on-failure -j 1 -R .
```

Debug 用独立 `debug` 目录和 `CMAKE_BUILD_TYPE=Debug`。ASan 用独立 `asan` 目录、RelWithDebInfo、
`CMAKE_CXX_FLAGS=/EHsc /fsanitize=address /Zi`，`ASAN_OPTIONS=alloc_dealloc_mismatch=1`。
实际执行入口是归档的 `verify.ps1 -Implementation <I>`，配置命令和退出码在各时间戳日志中。
额外消费者使用 `-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE`，检查没有 Vulkan/Native target，compile command
不含 VulkanSDK、Native、VMA 或 Legacy。最终 fresh relocated SDK 共 73 文件，完整哈希清单已保存。

25 个公共头从 I 的 Git blob 同步到 Debug、RelWithDebInfo、Android 三前缀，共 75 份；先备份原文件，逐个验证哈希。
备份/清单：`E:/RenderV2Rebuild/S0G10/sdk-backup/<I>/manifest.json`，并收进证据归档。
没有新增公共头或兼容转发头；未把用户未提交 Graph.hpp 混入 SDK。Android 仅同步头，不代表构建资格。

## 正确性和唯一所有权

| 门禁 | 独立验证事实 | 结论 |
|---|---|---|
| S0-C1 | 同一许可不可复制；测试审计伪造副本也被 retired/generation/owner 检查拒绝；Buffer/Image 重复退休、退休后直接销毁、跨 Session 同值 Key、容量满保留重试、Alias 错序/非法来源、真实 Timeout/提交失败后旧 Ticket 保持 | PASS |
| S0-C2 | 未声明受控延期能力的 GPU-ready Feature 无法注册；已声明能力的 Handler 直接普通 reply 仍返回合同错误、无 Success | PASS |
| S0-C3 | 零 Pass / 纯业务 Feature 不提供 Program 也可 Hello/Bind/Submit/Reply；有 Pass 缺 Program 拒绝，保留原有错误/last-good 合同 | PASS |
| S0-C4 | Binding 构造 Client-only，对外只读；原始 IPC Peer 伪造旧 generation/session 仍在 Server 边界拒绝 | PASS |
| G10 两阶段提交 | Reply schema/容量及固定槽先预留；Native submit 取真实 Ticket 后无二次容量失败地 commit；此前 Ticket 不能冒充本次提交 | PASS |
| 真消费者 | 测试模块注册开放 UploadProbe；原附件地址一致，真实 staging + destination + Copy + host-read barrier，实际 GPU 字节比较完成后才 Success | PASS |
| 延期与失败 | 完成观察 held 时无 Success、Owner 保留；Native 提交失败不丢前一次 Ticket；Completion 满/Reply 不足在提交前拒绝；Native 3 槽满正确拒绝且可恢复 | PASS |
| 来源与进展 | 两 Client 数值相同 RequestId 不串 Link；Main 64 未读 Reply 停顿 1.5 s，Upload 和四 View 持续推进 | PASS |
| 寿命 | abandon 不提前归还完整 1 MiB backing 预算；Requester close 不取消 GPU；原始源、Native pins 恰好释放；已读 GPU Reply 晚于 Server/Client 仍可访问 | PASS |
| 停服 | join 两个编译任务；处理 GPU + Readback；cold GPU drain 后释放 pins；所有 ReceivedRequest 在 Responder reset 前清偿；开放 Client 对已提交未确认业务收到 Unknown | PASS |
| 并发编译 | 两个 host worker 同时进入真实 Shader/Pipeline 编译，共用一个 DeviceSession，四 View 继续提交 | PASS |
| 既有行为 | G9 两 Scene / 四 View 像素、Native 生命周期、R5 相邻 Pass 优化、Client/SPSC shutdown 与全部旧测试保持 | PASS |

只保留一个 Operation SlotMap、一个 DeviceSession/VMA 账本、一个拥有延期请求的集合。
新增生产类型 5 个（按 class/struct/template 定义计，不含测试类型）：`AllocationPermit<Tag>`、
`OperationContext`、`GpuCompletionReservation`、私有 `GpuCompletion`、私有 `DeferredCompletion`；
删除 `PendingReadback` 1 个。另有 BufferPermit/ImagePermit 两个实例别名和一个 VCompletionCondition variant 别名。
没有新 Resource/Request Manager、Registry、线程、Graph 或 IPC 系统。

许可包含借用 Session + 既有 SlotKey，Session 必须活过许可及 record borrow。原 Record 是唯一 Active/Retired/Released
事实来源；外部退休先 owner/key/state/ticket/capacity 验证，再失效许可。Context 已准备指针热路径和复合 unique_ptr
退休路径不变。`releaseCompleted` 只处理实际已完成的 Buffer pins，未完成时返回 Busy、保留许可，不替代 in-flight 退休。

G10 Reservation 与 Context 均只在 Handler 调用期间有效；唯一 Pending 保存原 RenderRequest、真实 Ticket、一个 Native
pin owner 和预留结果。ready 回调只在完成操作时调用一次，不能进入稳定 Feature 每帧派发。先释放 Native pins，再发布
Reply；关闭后副作用不确定的已提交工作不会伪装成 Cancelled。此轮限定为 Session graphics queue 的真实上传/Copy 路径，
不宣称已实现 G11 专业 Storage、Dedicated Transfer 调度或其它 Backend。

最终矩阵 GPU Validation / SyncValidation **Error=0**、ASan **Error=0**。日志保留了实际 EOS Overlay 重复层 warning，
例如 Foundation warnings=1，故不宣称零警告。Fault 注入和完成观察控制只在测试目标；成功路径仍依赖真实 Fence 与字节结果。

## 尺寸、分配与性能

| 指标（Release） | 起点 → 最终 | 解释 |
|---|---:|---|
| IpcMessage / IpcReply | 144 / 296 B，未变 | IPC 生产源码完全未改 |
| Binding / ExecutionContext / RetirementEntry | 32 / 200 / 32 B，未变 | 未增加 per-binding 归属元数据 |
| BufferAllocation / ImageAllocation | 48→56 / 72→72 B | retirement 状态及对齐成本 |
| 10k ledger + binding | 1,302,680→1,343,640 B；61 次 new 不变 | 增加 40,960 B，未隐瞒 |
| AllocationPermit / Reservation / Context borrow | 16 / 24 / 40 B | 冷态许可，不复制进 Binding |
| RenderRequest / DeferredCompletion | 216 / 448 B | 固定 128 槽 57,344 B，旧 Readback reserve 28,672 B；测试 2 槽 896 B |
| 稳定四 View 窗口 | 534 ticks / 2127 submits，Engine new=0 | Debug 541/2134/0，ASan 492/1954/0 |
| Upload 1.5 s Main 满额窗口 | 514 次成功 | Debug 535，ASan 394；并非隔离吞吐基准 |
| 主 GPU 用例 Native Buffer create/destroy | 3052/3052 | 额外开放 Client 停服用例另建 2 个；最终计数相等且 live=0 的断言通过 |

R5：同 Shader 1 Pipeline / 1 Bind / 1 begin-end Scope / 2 Draw；不同 Shader 2 Pipeline / 2 Bind /
1 begin-end Scope / 2 Draw。非法合并的分隔情况仍保持两个 Scope，未为统计数字放宽屏障/合法性。

以下是独立 V Release 的 1000 样本，单位 **µs**，分位数采用 nearest-rank；所有原始行保留。

| CPU 小型已绑定 Operation | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| Submit + Encode | 0.6 | 1.6 | 2.4 | 11.0 |
| Handler + Reply | 1.1 | 2.5 | 6.2 | 11.3 |
| 成功 Consume + Decode | 0.9 | 3.6 | 11.9 | 43.7 |
| 提交返回至收到结果（含调度/消费） | 1986.2 | 2220.0 | 2544.0 | 2600.2 |

该完整小型 API 测量的 Engine new 全部为 0，不使用预编码队列样本代替。

| 真实 GPU-ready Operation（CPU 时钟） | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| 入站 → Submit accepted | 403.9 | 594.9 | 704.5 | 2276.9 |
| Submit → Fence 完成被观察 | 15595.7 | 16587.3 | 16996.2 | 24731.0 |
| Fence 观察 → Reply 发布开始 | 20.1 | 31.2 | 47.7 | 234.0 |
| Reply 发布开始 → Client 消费 | 14413.4 | 15308.8 | 15479.4 | 15797.2 |
| 入站 → 消费 | 30719.5 | 31860.0 | 32181.3 | 32604.3 |

发布时间是发布函数开始的边界，不是 SPSC 原子 store 时间。GPU timestamp **NOT_MEASURED**。
这些时段包含四 View 竞争、主机原有 `sleep_for(1ms)`、客户端轮询和调度；不能解释成纯 GPU 时间或 IPC 队列延迟。
未改变 sleep，也未声称 GPU 更快。Debug/ASan 端到端 max 分别 62.2513 / 62.2193 ms，异常行没有裁掉。
同 I 的实施环境与独立 V 时间存在明显运行波动，原始样本全部归档；没有等价受控对照，不给出“性能改善”结论。

GPU 请求 Release 每完成样本 Engine/test `new` 为 7（p50/p95/p99/max 均 7），包括测试输出 Owner、Runtime 消息等；
Native 资源为两个真实 VMA Buffer 分配/释放，驱动内部 heap 未测。Debug 每样本 new p50=11、max=19；ASan p50=7、max=12，
重试累计分配也计入样本。它们与稳定帧/小型 CPU 请求零分配是不同口径。
自然 Native 队列容量拒绝在独立 Release/Debug/ASan 分别为 0/7/1 次，另保存 `runtime-gpu-backpressure.csv`；
只允许准确的、提交前 capacity 错误有界重试，未将失败改成 Success。预留槽满和实际 Queue 满均保持有界背压。

## 失败记录、保护与剩余边界

实施期间发现并修复的夹具问题：move-only Owner 构造错误；错误保留旧失败 Reply 导致预算断言超时；
未考虑 Native 队列正常容量拒绝的无条件 Success 断言。诊断重复运行记录了确切容量错误码、入站时间、Submit/Fence=0；
后续受控实验复现并验证了无 GPU 接纳/保活丢失。没有通过生产算法猜测性优化消除失败。
失败尝试没有完整 CSV 时保留原始日志，不伪造缺失样本，不用于成功性能分位数。详见 implementation-evidence 下的失败日志和
`capacity-diagnosis.txt`。最终独立 V 未发生测试失败，也未在 clone 修实现。

`check_render_s0_g10.py` 使用 be9d7af3 精确白名单；旧 R0–R6 checker / 报告未修改。
三配置及最终复查均确认 Legacy **719/719**、其 manifest、**3099 个白名单外文件**与起点 Git blob 一致。
纯 Protocol、G2、G4/G5 算法、IPC SPSC/Channel/Link/credits/退休全部只读。
原用户工作树仍在 `f80902e100879b114e848cabe1469540aba4d25d`，仅原两处修改，未 checkout/reset/cherry-pick：

- `.vscode/settings.json` SHA-256 `f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`
- 用户 `Graph.hpp` SHA-256 `00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a`

根 TOOLCHAIN 实际 configure 退出 1：缺少 `render/client`、`render/features/cmake/feature_client.cmake`、
`render/standard_content`，并引发其安装 target 的级联错误。此历史产品闭包 BLOCKED 没有被 bootstrap 的 PASS 代替。
没有新增占位目标、链接 Legacy、禁用测试或跳过根配置。

Linux / Android 构建 / TSan：**NOT_RUN**。GPU timestamp、驱动内部 heap：**NOT_MEASURED**。
IPC R2 性能 **PARTIAL** 仍为原判定。本轮无未关闭的已复现双退休、错误 GPU Success、Owner 提前释放、Validation 或 ASan 错误。
未来消费者必须遵守 Session 生命周期、Handler-only borrow、Reserve→Submit→Commit 和 bounded capacity 合同；
本轮不扩展 G11–G13、生产资产 Storage、跨平台资格或通用 Backend 抽象。

## 精确文件边界

相对起点，生产/测试/本轮新增施工材料的完整变更如下（I 共 23 文件；本 V 仅另增本报告）：

```text
A	.internal/editor-redesign/check_render_s0_g10.py
A	.internal/editor-redesign/render-g10-implementation.md
A	.internal/editor-redesign/render-s0-g10-ownership.md
A	.internal/editor-redesign/render-s0-implementation.md
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/DeviceSession.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/memory/Memory.hpp
M	modules/function/render/native/vulkan/include/lux/engine/render/native/vulkan/retirement/Retirement.hpp
M	modules/function/render/native/vulkan/src/Execution.cpp
M	modules/function/render/native/vulkan/src/memory/Memory.cpp
M	modules/function/render/native/vulkan/src/retirement/Retirement.cpp
M	modules/function/render/native/vulkan/test/Foundation.cpp
M	modules/function/render/runtime/include/lux/engine/render/runtime/RenderClient.hpp
M	modules/function/render/runtime/include/lux/engine/render/runtime/RenderServer.hpp
A	modules/function/render/runtime/pinclude/Deferred.hpp
M	modules/function/render/runtime/src/RenderClient.cpp
M	modules/function/render/runtime/src/RenderServer.cpp
M	modules/function/render/runtime/src/Request.cpp
M	modules/function/render/runtime/test/CMakeLists.txt
M	modules/function/render/runtime/test/CompileWorker.hpp
M	modules/function/render/runtime/test/ControlFixture.hpp
M	modules/function/render/runtime/test/FeatureOperations.cpp
A	modules/function/render/runtime/test/GpuReady.cpp
M	modules/function/render/runtime/test/Programs.cpp
```

ALLOWED：上述受影响 Native/Runtime 接缝、必要测试和新 `.internal` 材料。
READ-ONLY：旧报告/checker、Legacy、IPC、G2/Protocol、Graph/Feature 算法、依赖仓库与用户工作树。
FORBIDDEN：G11–G13、第二 Owner/Manager/Queue、main 合并、强推、改历史资格、以 Stub 填补产品闭包。
