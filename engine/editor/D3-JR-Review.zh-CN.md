# D3 联合候选修正：终止、失败进度与动态 Run

本轮保留资源 owner、值解码、Main/worker、独立 Run 及 Editor 顶层结构。范围是 JR-01、JR-02、固定步长节奏、RunPane 错误表达和持续变化的 Run 联测，不增加运行模式或资源流送。

生产提交：`4466ed37f1e2944481b6f250d257e28b615d89a1`。
最终代码／测试候选：`67c6f0d0240555dd8927c37f84656b19f03aadb0`；后两个提交只补测试计时输出、最终结果观察点及关闭 Vulkan validation 的成本模式。
比较起点：`28523b4e68a4ae1c6d33659e30f96b2b752db998`；开始时实际 HEAD 为其报告提交 `ccf78d109ea3b027d5264b49b9ace60f13a7fbf8`。

实际开发检出 `E:/lux-ed-d2/src`，分支 `codex/editor-d3-foundation`，开始时干净，无适用的额外 AGENTS。没有覆盖未知修改。main 保持 `4aee84a48e235174534ca4a0fe0cc6afe39cfcd7`。
旧候选及报告在本轮开始前已经推送；旧报告中的“未推送”仅描述其形成时刻。本轮未推送、合并、发布或冻结。

## JR-01：将停止意图与真实退出分开

`RenderRuntimeLease::status()` 提供 ACTIVE、STOPPING、RETIRED 及原始终局 RenderError。只有 `EditorRenderer` 已观察后端退出、join 后端、取消已接纳 CPU frame、回收 Program ring 和 client staging，才报告 RETIRED。没有以 isStopping、引用计数或 noexcept 代替退出事实。

`SceneRenderBinding::poll()` 即使 packet budget 为零也推进生命周期：

| 事实 | 行为 |
| --- | --- |
| 正常背压 | 保留待提交包和 owner，等待重试 |
| STOPPING | 封住生产端并唤醒等待者；保留包、Scene/runtime lease；不再尝试新 drain |
| RETIRED，但 producer 未退出 | 继续保留 owner |
| RETIRED，producer 已退出 | 回收本地未转发包及准备资源；按已退出后端契约释放 Scene/runtime lease；进入 CLOSED |

终局退休有独立 `retired_unforwarded`／Run `retired_updates` 计数，不增加 forwarded，不伪造后端采用或 GPU 完成。最初的结构化失败仍在 Binding 和 Run 结果中。

正常关闭保留原有有序 Program 附件协议。另外补上最后一个 View 关闭后的有限推进：Program ring 的持久槽在复用时才释放附件，不能依赖 GUI 再画几帧。关闭期间每次 poll 最多接纳一个空 StateUpdate、最多一圈槽位；它不生成 Frame，不计为业务更新。仍按真实前缀退休后关闭 Scene，未增加 waitIdle、调度器或消息格式。

修前的两个准确负例均运行于旧 SDK：

| 模式 | 后端／worker／View | Binding | published / forwarded / pending | runtime leases |
| --- | --- | --- | --- | --- |
| 待转发 | FAILED／已结束／已关闭 | CLOSING | 1 / 0 / 1 | 2 |
| 已转发，drain 未提交 | FAILED／已结束／已关闭 | CLOSING | 1 / 1 / 0 | 2 |

旧驱动对这个具体状态断言失败。它不是任意崩溃或超时通过，也没有宣称旧程序正常退出。为隔离 Binding 缺口，旧驱动显式停止 worker；新驱动要求消费者终止自动唤醒 worker。

修后两条路径 CLOSED，pending=0，forwarded 分别保持 0／1；移除测试观察者自己的 lease 后总数为零。另有一个尚未提交的 client staging 附件，验证 STOPPING 时不销毁、真实后端退休后恰好销毁一次。完整动态 Run 终止用例还检查 pins=0、作者不变及原始 ChannelStopping 错误。终局来源是既有 channel 的显式终止错误，**不是物理 device-loss 实验**。

## JR-02：最终进度与结果同时交付

worker 的拥有型 `RunCompletion` 包含 observation、failed_phase 和 `EditorResult<void>`。失败不再用 unexpected 丢掉本地 observation；外层 expected 仅保留既有 Process 完成机制。

Simulation 执行返回后，先读取实际 clock，再判断执行结果。`steps/elapsed` 表示已经采用的 clock；`completed.simulation/stable/publication/presentation` 分别表示最后成功的阶段步数。必要发布等待的时间单独累计，退出前累计已经发生的主动工作。

| 实际负例 | 旧结果 | 新结果 |
| --- | --- | --- |
| 首次 stable 失败，system=2 | steps=0，elapsed=0，work=0 | steps=1，elapsed=10ms，simulation=1，其余成功阶段=0，failed_phase=STABLE；保留原 SceneExecutionFailure |
| 真实 Simulation task 在下一步失败，system=3 | 本轮新增组合，无旧版复现声明 | clock 比暂停点增加 1，成功 Simulation／stable／presentation 停留在暂停点；failed_phase=SIMULATION；保留 SYSTEM_TASK_FAILURE/system=3 |

Main 上最先得到的 Binding 终局错误不会被 worker 后续停止的次生结果覆盖。Binding 最终释放前再读取运输统计，避免最终 pending／retired 留在上一次 poll 的值。

## 节奏和 RunPane

固定 simulation delta 不变。下一次期限从本步开始时刻加 delta 计算，工作耗时包含在周期内；若已经落后，则丢弃积累的节奏债务，允许下一步立即开始并重新建立期限。没有追赶循环、重复执行同一步或可变 delta。Pause/Resume 重新建立锚点。该政策不承诺硬实时，也不会让慢于 delta 的计算保持现实 1 倍速。

RunPane 在局部拥有原 RendererFailure，包含 code、View 代次、request、backend error／status；相同错误不在每帧重新分配。区分 NOT_READY、明确失败和关闭等待。失败 View 不被每帧 resize 覆盖；显式 Reopen 先完成旧 View 关闭，再建立新的 View，不停止独立 Run。关闭错误保留 owner。

本轮有 RunPane 正常生命周期回归和源码路径核对；**没有宣称逐一通过物理输入触发的 RunPane 错误展示／重开验收**。

## 真正持续变化的运行场景

测试注册一个真实 Simulation system：每步按实际 delta patch Transform，并有界地增删一个 Light，使用既有 TransformSystem、提取 stage、Process worker 和 Main 消费。没有 worker Main-client 调用，也没有手工 producer 替代演进。

同一条流程检查：连续 StateUpdate；暂停后 clock 不变；Step 恰好一次；后端 Light 数量与暂停步奇偶一致；Main 暂停 Binding 消费时，producer 在稳定点等待而 GPU 完成水位继续前进；Stop 不依赖恢复消费即可唤醒 producer；关闭 View 后停止绘制仍能结束 Run；作者 Transform、历史 StateId／revision／cursor 不变。

另一模式在背压点终止后端，联合验证实际 Run 的 worker、View、Binding、pins 和错误。不是把运输测试、静态 Scene 和 CPU owner 测试的数量相加。

本轮是隐藏原生窗口中的真实 Vulkan／GPU 流程、程序化控制；GPU 完成不等于物理屏幕呈现，也不替代鼠标手势或 IME。

## 工程资格与证据

最终 42 项安装 CTest、9 个独立组件消费者、直接 owner 程序和生成器拒绝／输出保留检查均通过。详细资格及成本数字由本报告同包的 `qualification-summary.json`、`acceptance-results.csv`、原始日志和 manifest 给出；只有完成的命令才登记 PASS。

- 仅 RelWithDebInfo。复用独立资格 clone `d3q/s` 和其增量构建 `d3q/b`，检出固定候选且保持干净；不称为新建全量构建树。
- `all -j 4 -- -k 0` 及第二次 no-op；新安装到 `d3jrq/sdk`，复制到 `d3jrq/relocated`；新消费者构建排除 work-sdk、旧 SDK 和旧构建运行路径。
- 42 项安装 CTest、9 个独立组件消费者、直接 owner 程序、生成器缺失组件拒绝分别记录，统计单位不相加。
- 正常 SDK 的 UI／publication diagnostics 为 OFF，无本轮 replacement new/delete 或故障注入路径。原故障诊断资格不重贴成新候选通过。
- Git blob OID、Git blob SHA256、Windows checkout SHA256 分开记录；SDK、DLL、EXE 和原始证据各有哈希。

开发中先运行受影响的 9 项测试，固定候选后统一运行资格。计时仅在构建／测试结束后串行执行。最后测试提交只补输出和成本模式，最终生产与消费者的第二次构建均检查 no-op；没有拿未提交修改的日志换候选标签。系统析构与整个 worker 完成的计时观察点在测试中分开；`pre-final94` 的旧 `stop_producer_us` 实为系统析构，保留原日志并明确纠正口径，不当作完整 worker 结束。

旧 SDK 的三个准确负例在 `before-terminal-pending.log`、`before-terminal-forwarded.log`、`before-progress.log`。此前驱动的 metadata 装配遗漏、空资源快照访问、错误 feature 名和编译／启动错误另行分类，不算产品复现。关闭实验中的 deadline 日志属于调查过程，不被当作准确终局负例。旧有效原始证据不删除、不改标签。

## 有限成本和保留范围

五次独立进程使用相同四对象输入、固定 10ms delta、相同 View 和人为暂停 Main 消费的节奏。关闭 Vulkan validation 后记录逐步主动工作（Simulation、资源验证、stable、presentation；不含初始装配）、发布等待，以及 Pause→稳定暂停、Step→完成、Stop→Main 收到 worker 完整最终结果、Stop→Run 资源退出四项延迟。每个样本仍记录实际步数／发布量。驱动按约 1ms 轮询，并主动暂停 Main 消费 140ms；延迟包含驱动节奏和真实资源收尾。它们是控制延迟观察，不是逐操作 p95、FPS 或新旧性能比较。

本轮五次均完成 7 个实际模拟步、发布 6 包、1 次必要发布背压。下表为中位数；五个原始样本均在 `cost-control-latencies.csv`，没有剔除冷进程。

| 观察量 | 中位数 |
| --- | ---: |
| Pause→稳定暂停 | 1.031 ms |
| Step→完成 | 1.070 ms |
| Stop→Main收到完整worker结果 | 2.556 ms |
| Stop→Run资源退出 | 90.797 ms |
| 逐步主动工作累计 | 0.297 ms |
| 必要发布等待累计 | 129.883 ms |

旧四对象结构操作五组配对、资源关联规模样本保留旧候选身份。本轮没有改结构编辑算法，也没有把这些旧数字用于新 Run 的速度结论。结构删除仍扫描已知作者组件并做引用编码验证；不声称只与修改对象数量相关。尚未新增大场景结构编辑的扫描／深层字节完整计量，未为计量增加生产热路径拦截。

D2-C 的 C19 双 Pane／排队通知组合、C24 物理持续拖动取消与局部文本 Ctrl+Z、旧桌面 native-close 发送者来源继续保持原状态。没有用本轮成功改成通过。

未开展插值、外推、自动重同步、动态流送、新时基、MCP、Launcher、无窗口产品、机器人／SLAM 或全工程 OOM 改造。完成此有限修正波次后提交独立审阅，不宣布 D3 完整产品验收通过。
