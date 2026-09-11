# V4-NA1 实施结果：短任务有收益，长任务与 record 代价上升

## 结论

实施 **IMPLEMENTED**；合同 **TESTED_WITH_EXPLICIT_COVERAGE_GAPS**；成本 **MIXED_NO_GENERAL_GAIN**；采用 **NOT_APPROVED_PENDING_REVIEW**。

真实 C++ coroutine 编排、同步 Lua 步骤、组合 backend、Physics 目标迁移和安装纵向链已实现。P1/P2/P3 的完整任务更快，P4/P5 明显更慢，混合 Physics 整帧也未获收益。保留原 Lua coroutine 产品路径；不建议据此全面迁移。没有继续调整 GC、VM、allocator 或框架救成绩。

已跑完规定的 54 个有效正式进程、6 次有效 ROI 软件采样、13 个独立资源观察、Developer/Toolchain/VM 及安装资格。54 条逐项登记中仍有明确覆盖缺口：冷期每个分配点未逐点注错、主栈扩容 OOM 未独立触发、部分 owner 元数据未单独归因。它们没有被写成 PASS，见 [CHECK_MAPPING.csv](CHECK_MAPPING.csv)。当前没有被已执行测试证实但未修的生产合同失败；这不等于已证明全部边界无缺陷。

## 身份与实验隔离

| 用途 | 身份 |
|---|---|
| A 生产/原资格 | `f7d2815bdd2025ee23a7c11449def822413f58e9` 匹配 V4 镜像 |
| 分支起点 | `f92853ea8361a7dc90ce0da072497d3722c29f88`，相对 A 仅报告/证据 |
| 共同 fixture | `4a8812e9813fe90b241dba29e3e668e4a4239b26`，A/B 分别链接旧/新 SDK |
| B 生产及独立 clean clone 资格 | `4a8812e9813fe90b241dba29e3e668e4a4239b26` |
| 分支 | `codex/s6-native-task-lua-steps` |
| lux-cxx / toolset | `3100f54d0743c5ed94a4ccf5943df04e933de255` / `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f` |
| Lua | 5.5.1 + lux-lua55-v3-r2；INC 上游参数；16 MiB 完整空页预算 |
| 配置 | Windows x64、MSVC 19.44、RelWithDebInfo、/O2 /Ob1 /MD /Zi、Native ABI6 |

本文及证据提交不重标为新测试源码。VM DLL SHA-256 为 `34f11df8679a46b2d956462a99a9a3ba49ee29e88b85bb27b1c2b3c5a78bb29e`。204 个固定安装依赖文件在开工/收尾哈希一致；安装身份没有拿源码 HEAD 代替。A/B 两套共同 fixture 的 30 个公共 DLL 与各自固定镜像逐项匹配。

隔离路径为 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/script-native-lua-na1/`，其中 `source` 为实施分支，`qualification-source` 为独立 clean clone；复用 `o/w/d`、`o/w/t` 两个构建槽。B 新前缀为 `install/o/na1/qualified-sdk` 与 `qualified-tools`；A 使用既有 `install/o/v4/sdk`/`tools`，不混 H1/A1。

## 实际调用链与作者合同

`ScriptSystem → 原 PreparedInvocation → facade 的映射 CppStatic start → 真实 C++ coroutine → 原 Event/Timer 登记 → 原 ScriptExecution 稳定点 → CppStatic resume → context.callStep → Lua main_thread 同步 protected call → 原 Lua 业务 → scalar/void → frame 完成/销毁`。

一个核心 mount/完整 identity/behavior，两个 backend 子对象。`NativeLuaTaskPlan` 冻结 Lua/Cpp 制品身份、任务 contract、路由、步骤和 import 映射；未映射导出走原 Lua，映射失败报错不 fallback。创建按 Lua child→步骤→Cpp child 发布；失败逆序回滚；EndPlay、执行取消和实际回收仍由原核心时机控制。

公共变化为 `ScriptSyncStep.hpp`、默认空 `ScriptInstanceCreateContext::sync_steps`、`context.callStep<Signature>`、`context.fail` 及安装组件 `simulation_script_native_lua_tasks`。旧 C++ SDK 消费者须重编译；ABI6 不代表新旧 C++ EXE/DLL 可混用。同步返回 expected 的临时值在源码中置于普通 helper；后续机器码审计发现部分暂存经 MSVC 内联仍进入 P4 的 coroutine frame，不能以源码作用域宣称物理上全部在普通栈。跨等待值由编译器 frame 持有。

同步事务保留内层 checkstack、当前/原发布资格、原错误保护及转换后重验。引擎 await 在登记前拒绝；raw yield 不能跨同步 C 边界；Lua 内部 pcall 可合法恢复。fail 保存显式错误并终止当前任务，不分配 A/Ready、不继续业务。原 TaskGraph、预算、真实 step、frontier、single-flight、Event 多飞、Event/Timer 不同满队列政策和 pin 均未改。

这是新的显式作者模式：不保证单体 Lua coroutine 的 thread 身份、挂起调试栈和任意 Lua 局部对象跨等待存活。没有透明线程池/每任务 Lua context table。具体所有权、构造和清理见 [NOTES.zh-CN.md](NOTES.zh-CN.md)，业务资产映射见 [fixture_map.json](fixture_map.json)。

## 结构和资源

开启观察的 B P1/P3/P5 中，任务 Lua thread 创建/resume/release 全为 0；混合旧路由负对照仍观察到真实旧 Lua thread。任务 root/yield 的零来自未进入 coroutine 获取链和同步非法等待拒绝的联合证据；没有独立 root/yield 计数器。正式计时关闭 allocator/leaf 统计表示未采集，不表示零。

实际编译器 frame：Event 176 B、NextStep 240 B、三等待 352 B、32 等待 352 B、ValuePose 512 B，alignment 8。原池上限 512 B 保持。16 个 record 的超限任务在进入 body 前拒绝；这个早期 guard 不增加存储 allocator 的 capacity-failure 计数。实际 frame 小于预留区域不意味着最坏 backing 已同比减少。

P1、10,000 实例，独立资源组（16 warmup、32 波，非正式计时）：

| 指标 | A | B |
|---|---:|---:|
| prepare 后全部 heap busy bytes | 63,343,906 | 95,702,697 |
| 计时结束全部 heap busy bytes | 83,861,026 | 95,707,049 |
| 计时结束 working set | 90,275,840 | 103,251,968 |
| 此时已观察 peak working set | 118,267,904 | 103,251,968 |
| prepare 后 VM live bytes | 11,228,495 | 11,138,443 |
| 结束 active-page backing | 32,505,856 | 21,102,592 |
| 结束 idle-page backing | 9,109,504 | 0 |
| VM 下层 heap alloc/free 累计 | 3,916 / 3,279 | 328 / 5 |
| VM thread/resume/release 累计（含 warmup） | 480,000 / 480,000 / 480,000 | 0 / 0 / 0 |

B 已测固定子项：composition 10,641,296 B；native frame arena 5,760,000 B；frame metadata 480,352 B；native prepared 800,000 B；association 960,008 B。**这五项小计 18,641,656 B 不是全部 backend 总数**，还需 Lua/Cpp 对象、目录等。B_KEEP_LUA_RESERVE 同规模 prepare heap 比 B 多 811,366 B，包含推导的 720,000 B continuation/free-index 向量和 90,052 B VM live 差；没有创建任务 thread。

[RESOURCE_LEDGER.json](RESOURCE_LEDGER.json) 覆盖 1,000/10,000 实例所有阶段、core/source 主要 backing、各 owner 剩余目录和当前编译器布局。全部 heap 有实测总量；部分 owner 哈希桶/节点未单独标记，精确 owner 总和保持 null。VM live、page backing、slack、whole heap、RSS 属于重叠口径，不相加。closed 快照在 ScriptSystem 关闭后、backend/VM 对象析构前，保留的容量和等待正常 GC 的死对象不应直接称泄漏。

## 完整业务性能

每腿三对 AB/BA/AB，固定 workload、worker=0、affinity mask=16、normal priority、原来源时序和 budget=N。共同 fixture 固定 payload 无随机数；旧驱动 seed=1592598566。每组独立进程，未删除有效慢组。下面 A/B 是各侧单位成本中位；最后列是逐对 B/A 变化的中位，二者算法不同。

| 场景 | 单位 | A 中位 | B 中位 | 三对变化 | 配对中位 |
|---|---|---:|---:|---|---:|
| P1 单 Event | ns/完整任务 | 642.791 | 532.445 | -16.54% / -17.17% / -19.03% | -17.17% |
| P2 NextStep | ns/完整任务 | 685.980 | 645.753 | -8.65% / -5.17% / -4.43% | -5.17% |
| P3 三等待 | ns/完整任务 | 1726.517 | 1465.118 | -10.55% / -15.44% / -15.61% | -15.44% |
| P4 一任务 32 次 Event | ns/完整任务 | 11454.030 | 14774.544 | +23.90% / +28.88% / +29.41% | +28.88% |
| P5 ValuePose 跨等待 | ns/完整任务 | 1115.934 | 1660.836 | +45.91% / +48.83% / +53.30% | +48.83% |
| S1 原 Lua scalar | ns/调用或任务 | 130.730 | 132.260 | -6.15% / +1.45% / -2.28% | -2.28% |
| S2 原 C++ Sequence | ms/原帧 | 2.518 | 2.367 | -1.43% / -6.02% / -2.32% | -2.32% |
| S3 原 FlowForge Event | ns/调用或任务 | 311.488 | 317.669 | +2.22% / +1.23% / -2.74% | +1.23% |
| P6 混合 Physics | ms/原帧 | 2.792 | 2.900 | +0.14% / +4.20% / +3.87% | +3.87% |

S1 的 B 单侧中位略大，但配对变化中位为负，属于配对和非配对统计差异；不能据此宣布等价或稳定改善。S2/P6 沿用原整帧区间，最终 shutdown 在原区间之外，不把它们称为“全部任务完成成本”。P1–P5 包含任务创建、完成、销毁及必要 drain。准备成本另外保留；没有把伴随制品和准备成本隐藏在另一架构中。

每个完整 ROI 总秒数 A→B：

| 场景 | 第一对 | 第二对 | 第三对 |
|---|---:|---:|---:|
| P1 | 13.246194 → 11.055720 | 12.855814 → 10.648892 | 12.547511 → 10.159560 |
| P2 | 14.079961 → 12.862548 | 13.719605 → 13.010163 | 13.514037 → 12.915060 |
| P3 | 4.094843 → 3.662796 | 4.316293 → 3.649956 | 4.351826 → 3.672658 |
| P4 | 7.050015 → 8.735182 | 7.336767 → 9.455708 | 7.330579 → 9.486617 |
| P5 | 1.124762 → 1.641140 | 1.115934 → 1.660836 | 1.101280 → 1.688234 |
| S1 | 2.818525 → 2.645204 | 2.614595 → 2.652461 | 2.561076 → 2.502693 |
| S2 | 2.391921 → 2.357798 | 2.518406 → 2.366810 | 2.632233 → 2.571082 |
| S3 | 3.107566 → 3.176694 | 3.140439 → 3.178967 | 3.114882 → 3.029545 |
| P6 | 5.274584 → 5.281836 | 5.589498 → 5.824349 | 5.584968 → 5.800843 |

P1/P2 各 2,000 万任务；P3 250 万任务/750 万等待；P4 64 万任务/2,048 万等待；P5 100 万任务。每个实例独立 readback，实际任务/等待/resume/provider 数及最终资源/错误/backlog 通过。原三腿/Physics 核对完整原业务列。每批 p50/p95/p99、prepare 时间和所有进程记录在 [result.json](result.json) 及原 CSV。没有独立 resume 延迟数据；每波 errors/backlog 和旧路径未暴露字段仍为 null。

## ROI、机器码和成本归因

P1/P3 两侧及 P5 两侧各一份有效软件 Hotspots。ITT 在真实 warmup 后 resume、完整计时波结束后 pause；业务 oracle 在外。首次四份采样未保存目标 stdout，缺少业务证明，按无效观察保留；补采的六份具有 ROI 标记、业务输出、exit0 和原 DB。无 PMU，不声称已证明 cache miss/branch misprediction。

- P1 A 可见 table GC、thread stack 初始化、页 acquire/release、lua_resume；B 删除任务 thread 生命周期，转为 Cpp acquire/resume、Event 解析和同步桥。B 的 C++ invokeSyncStep 包含时间约 26.61%（Lua invokeSyncStep 20.71% 为其子项，不能相加）。A traversestrongtable 自耗 .835655 s；B 该任务下 luaH_Hgetshortstr 自耗 1.301593 s，来自同步步骤内 Lua 执行。此差额的字符串布局/缓存成因没有单因素证明，不归为必要安全成本。
- P3 B 仍有原核心 ResumeOne、prepareResume、Event 解析/来源释放和 Lua 执行；少了 Lua coroutine 控制开销，完整任务中位 −15.44%。三次等待不能把全部整任务成本归到一次 resume。
- P4 双方都只创建一个任务、等待 32 次。A 已摊薄 thread 创建；B 每次恢复仍跨 typed step、原资格/pcall 和 Lua 调用边界，因此减少的生命周期成本不足以抵消新增边界。完整任务 +28.88% 是实测；其中各边界的精确因果比例未单独分解。
- P5 A 原 invocation 封送一次 ValuePose 并在同 thread 保留；B 在 before/after 同步步骤各封送一次。采样 A writePlan 包含约 45.36%；B 两个互不嵌套阶段分别约 19.72%/19.56%。百分比降低不表示绝对转换成本降低；B 多了步骤进入/退出和重复 table 构造。完整任务 +48.83%，没有改 Lua 运算或取消安全掩盖它。

反汇编、PDB 身份和当前编译器 class layout 均交付。PDB 可能含旧增量类型记录，当前尺寸另用同源码 /Zs /Z7 /d1reportAllClassLayout 取得，未据旧类型名首个匹配推断布局。同步 scalar 为一个外层 pcall，内部 lua_call 由其保护；record 的既有受保护 codec 子调用仍在，未宣称一层 pcall 处理整个任意转换。显式 fail 返回码和 frame cleanup 同时有机器码与真实负例。

## 构建、安装与合同验收

| 项目 | 实际结果 |
|---|---|
| Developer all / no-work / CTest | PASS；127/127 |
| Toolchain all / no-work / CTest | PASS；110/110 |
| 固定 Lua55 VM 合同重跑 | PASS；4/4 |
| 原消费者重编译运行 | PASS；15/15 |
| 新 native-task-lua-steps 安装消费者 | PASS；1/1；真实生成/打包/provider/结果/失败/取消/关闭 |
| 原生成增量 | PASS；13/13 |
| 新步骤签名/contract/映射/删除步骤增量 | PASS；4/4；恢复后运行及 no-op |
| 原两条 + 新 consumer 迁址 | PASS；3/3 |
| 补充真实合同诊断 | PASS；A/B Event 双满优先级；重复路由/步骤；嵌套 TBC；旧内容任务保活 |

原 Scene 六项协议断言本次全量通过；其预期修正在之前阶段完成，不冒称本轮修复。核心 Event cutoff、pin、Timer retry、frontier、stale budget、single-flight 多方法隔离、普通 Event 多飞、晚 C 接纳的现有测试保留。新增测试还覆盖 -771/-772、OOM、撤权、合法恢复、post-call publication invalidation、32 churn 和 frame 超限。

首轮 Toolchain 架构正例因复用槽内嵌套 probe 缓存仍指向 H1 source 失败，相关首轮负例不计资格。对每个 probe `cmake --fresh` 后，正例与预期拒绝原因核实并全量重跑通过。一次递归删除旧 probe 缓存被自动审批拒绝，未执行；使用正常 fresh configure 完成，不绕过拒绝去删除。

installed 使用 public SDK 和实际 tools；迁址时原 qualified source/build/SDK/tools 暂不可用并在 finally 恢复。公共变化位于 engine 头，新 SDK 已完整安装；本轮未改 modules 公共头，三旧前缀同步规则未触发，也没有把实验 C++ 接口写入 main SDK。没有 Android 构建或旧 VM 矩阵。

## 八项问题与停止决定

1. **有没有消除任务 Lua thread？** 有，开启观察的线程创建/resume/release 为零，no-Step 同步路径没有任务 root/yield 链；仍保留 Lua self/prototype/函数根和旧异步路由。
2. **总 backing 是多少？** 主要 owner 实测、编译器尺寸与整体 heap 都已给出；精确分 owner 的全部总和尚未获得。不能用 18,641,656 B 子项或 frame176 B 冒充全部资源。
3. **P1 赢，P4 还赢吗？** P1 配对中位 −17.17%，P4 +28.88%；长任务不赢。
4. **record 重复封送抵消收益吗？** 当前 P5 是，+48.83%。这是路线在此作者拆分下的负结果，不靠改变 record 语义规避。
5. **Physics 有整帧收益吗？** 没有，+3.87%；原 FlowForge Event +1.23% 配对中位且方向混合，旧未改路径未获得等价证明。
6. **不再保证哪些旧能力？** 原整体 Lua coroutine 的暂停栈/thread 身份及任意 Lua 局部对象跨等待保留，不属于新作者模式承诺。普通独立 Lua coroutine 产品路径仍可用。
7. **原 owner 协议还在吗？** 是；核心/来源/调度代码未因本实验增加第二套；新增的是组合子对象与同步步骤视图。错误、取消、失败、生命周期仍有原 owner。
8. **什么未知？** PMU 因果、独立恢复延迟、全部分配点注错、主栈 OOM 专门分支、精确 owner 整体归因、其他 OS/CPU/编译器/Android。残余不被解释为都必要，也没有启动另一轮优化。

建议保留该实验分支供审阅。可考虑少量等待、scalar 同步步骤的显式业务使用，但不自动推广；P4/P5/Physics 不支持全面替换结论。停止于 NA1 统一审阅。

## 保护、失败记录与证据

首批仅删除已核定旧 LuaJIT/Lua54 构建树，共 14,721,861,914 B，并保存配置/日志/目标边界审计。V4/H1 工作树 HEAD 和状态保持；main 的 7 项未知状态与已捕获 6 项内容哈希保持。期间 main 被别处 Editor 分支 fast-forward 到 f89e1862，见收尾 reflog；本任务没有操作该 merge，不能把 main HEAD 写为前后一致。原 .gitignore 未单独捕获开工哈希，只有状态可比。

早期失败包含 expected C++20 推导、安装导出遗漏、过大 frame、测试未推进真实 step、原 probe 缓存、采样 stdout 和诊断脚本错误；原日志均保留。修正都已进入连续子提交；失败测量不混为正式样本。

[证据入口](EVIDENCE.zh-CN.md) 提供归档、逐文件 SHA、产物身份、固定远端提交和回取检查。原测量身份始终为 4a8812e9/f7d2815b，报告提交不代替它们。大型 EXE/DLL/PDB 留在受控镜像，原 VTune DB 随证据保存。代码只推送实验分支，不合并 V4/main、不发 tag。

后续只读检查：[P4 直接采样](P4-VTUNE-FOLLOWUP.zh-CN.md)与[32 项热路径操作审计](P4-OPERATION-AUDIT.zh-CN.md)。这些补充未修改4a生产实现，也没有重标本报告的原资格/计时身份。
