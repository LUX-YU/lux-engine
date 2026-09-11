# NA1 热路径修改：实际结果

实现与本轮规定验证完成；目标长任务在当前同量配对中改善。采用状态仍为 `NOT_APPROVED_PENDING_REVIEW`。
没有合并 V4/main，也不把原 NA1 相对 V4 的债务注销或宣称性能等价。

## 身份与实际修改

- 工作起点：`96eef6246c96d86d5a5f51d2ca7c7a2ce8331d25`。
- 本轮修改前生产对照 B0：`4a8812e9813fe90b241dba29e3e668e4a4239b26`，原 B/B-common/B-flow 镜像。
- 最终生产与独立 clean clone 资格 B1：`21d106014e05cf0a7746d45fd05eac8ae01575f2`，新 C/C-common/C-flow 镜像。
- 代码提交：`0cc9aee2` 直接 typed 步骤与等待关系；`633c41c5` 保持合法无状态 publication callback；
  `21d10601` 冷期选择零参数/单 scalar 适配器。后续文档提交不是新的测试源码。
- lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`、toolset `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`；
  204 项实际安装依赖 hash 核验一致。Lua55 5.5.1/lux-leaf patch、INC、16 MiB、Native ABI6、MSVC RelWithDebInfo 参数不变。

[调用与安全合同](HOTPATH-CHANGES.zh-CN.md)和[32 项逐项处理](HOTPATH-OPERATIONS.csv)是本轮新增记录；
[原审计](P4-OPERATION-AUDIT.zh-CN.md)、[原资格](RESULT.zh-CN.md)与全部原始哈希保留。

主要删掉：每调用静态 FNV、通用 ABI value_slot/call_frame/passes 往返、两边的逐参数布局/对齐循环、
第二次资格 capture、相邻重复 valid、成功→错误结构→再检查、scalar 同步步骤的 C request/trampoline、
重复恢复数据解析、Cpp Event 的局部→实际 import 反复映射，以及 prepared Event 的重复静态布局检查。
不是只加 inline；没有新 scheduler、结果池、宽 ready ticket 或缓存 ACTIVE 跨用户代码。

lux-cxx 的 `TFnv1a`/`fnv1a` 确已存在。本次强制既有 semantic TypeId 在 consteval/constexpr 求值，
没有新增 hash，也没有把不同 unsigned-byte/零值规则的 hash 替换进去。

同步步骤作者现须声明 `SyncStepShapes`，生成 contract 与实际 Lua export 冷期比对。
公共 C++ prepared 输入形状有变化，真实任务、Physics、模板与安装消费者全部重编译；不保留旧 fallback。
跨 module 搬运调用模板/contract/context 不是支持入口。原 Native C ABI 与脚本 wire 不变。

## 同量成本

原九腿、每腿三对 AB/BA/AB，共 54 个有效进程。P1 Event、P2 NextStep、P3 三等待、P4 32 次 Event、
P5 ValuePose、S1 Lua scalar、S2 C++ Sequence、S3 FlowForge Event、P6 mixed Physics。
原实例数/预热/计时/seed/worker/affinity/预算/资产不变，完整任务包括必要 drain，正常 GC。
两侧 BUSINESS、逐实例结果以及现有 benchmark 每行业务字段一致；全部有效样本保留，无正式无效或丢弃组。
表中 A/B 是各侧每实际操作成本中位数；变化是三个配对比值的中位数，两者不必严格相除相等。

| 场景 | 修改前 | 修改后 | 配对中位变化 | 三对变化 | 单位 |
|---|---:|---:|---:|---|---|
| P1 | 0.516093 | 0.420270 | -18.57% | -18.09% / -18.57% / -19.78% | μs/完整调用或任务 |
| P2 | 0.646240 | 0.564146 | -12.50% | -13.06% / -12.50% / -11.94% | μs/完整调用或任务 |
| P3 | 1.401247 | 1.331865 | -5.01% | -6.02% / -5.01% / -4.87% | μs/完整调用或任务 |
| P4 | 12.456756 | 9.519093 | -23.05% | -21.27% / -23.05% / -23.80% | μs/完整调用或任务 |
| P5 | 1.461452 | 1.402759 | -4.02% | -5.74% / -3.13% / -4.02% | μs/完整调用或任务 |
| S1 | 0.120312 | 0.121172 | +0.72% | +1.92% / +0.72% / -1.38% | μs/完整调用或任务 |
| S2 | 2.258772 | 2.243157 | -0.98% | -2.04% / -0.98% / -0.42% | ms/原帧 |
| S3 | 0.279093 | 0.263435 | -5.61% | -5.61% / -6.93% / -4.32% | μs/完整调用或任务 |
| P6 | 2.510664 | 2.282613 | -9.09% | -8.00% / -9.09% / -9.21% | ms/原帧 |

P4 每进程 640,000 个完整任务、20,480,000 次等待/恢复，时间三对分别为
7.669→6.038、7.972→6.134、7.995→6.092 秒。不能把 9.519 μs/32 得到的摊销值称作单独 resume 成本。
P1 20,000,000 完整任务；P3 2,500,000 任务/7,500,000 等待；P5 1,000,000 完整任务。
原始批次 p50/p95/p99、总时间、prepare、业务量和全部分布见 [机器可读结果](hotpath-result.json)及归档。
恢复延迟/逐批错误/正式计时分配未单独采集，保留 null。
P1–P5 观测错误、backlog、lease 均为零；其他腿按原驱动实际错误观测范围记录，不把缺失日志字段写零。

S1 小幅双向波动，中位 +0.72%，没有性能等价结论。没有用其他路径收益抵消或抹掉它。
本轮未重新以 V4 跑九腿；旧 f7d2815b→4a8812e9 的 P4 +28.88%、P5 +48.83% 仍是历史数据，
不能与本次不同时段数字直接拼接来证明新候选胜过 V4。混合 Physics 不是各 backend 的独立成本。

## VTune 与机器码

P4 一对 software Hotspots，ITT 从准备/预热后开始，在全部计时波完成后暂停；两侧实际业务同量。
ROI 7.781852→6.158818 秒；采样 CPU 7.481614→5.793925 秒。不是 PMU，未证明 cache miss 或分支预测原因。
主调用树中（inclusive 不可相加）：

| 节点 | 修改前 inclusive CPU | 修改后 inclusive CPU |
|---|---:|---:|
| Tasks::step | 2.771541 s | 1.206055 s |
| ScriptCoroutineContext::invokeSyncStep | 2.488284 s | 1.176432 s |
| LuaScriptBackend::Impl::invokeSyncStep | 2.026663 s | 0.864339 s |
| lua_pcallk | 1.674777 s | 0.610839 s |

当前 scalar 栈不再经过 executeSyncStep→lua_call；保留一个真正 Lua 函数的 pcall。
Cpp resolveEvent 主路径 self 0.222800→0.074889 秒；当前额外 start 分支另见原树，不能遗漏或重复相加。
同时 luaH_Hgetshortstr self 1.047657→0.378006 秒，它的实现并未修改；这部分变化没有 PMU 因果解释，
不能把总差额全部归因于删检查。prepareResume 的采样 self 反而 0.162861→0.238674 秒，亦如实保留；
单对采样不足以把布局/调度采样变化归因成独立回退或新优化项目。

最终 COFF：Cpp invoke 与单 int→void Lua adapter 无参数 divq；typed-slot helper 消失；
await_suspend 的 FNV prime/循环消失，改为期望 TypeId 常量。零/单 scalar adapter 没有参数循环或动态 push 分派。
当前正常 Entity int→void 源码/API 路径为 checkstack、gettop、push traceback、两次 rawgeti、pushnumber、
pcall、settop，共 8 次；原 14 次。错误分支另计，API 次数是静态路径推导，不是采样计数或分配次数。
当前资格 capture 一次、原资格 valid 三次、publication callback 两次；实际 ordinal/identity/权限仍在边界检查。

## 布局和资源代价

| 实际 coroutine frame | Event | NextStep | 三等待 | 32 等待 | ValuePose |
|---|---:|---:|---:|---:|---:|
| B0 字节 | 176 | 240 | 352 | 352 | 512 |
| B1 字节 | 224 | 288 | 416 | 224 | 304 |

以上是同一最终优化产物 allocateFrame 的立即数，不凭源码 helper 推断全部暂存落在普通栈。
512 B frame 上限和完整预留没有缩小；10,000 实例 frame backing 两侧都是 5,760,000 B。
PreparedScriptSyncStep 24→32 B，LuaFunctionBinding 120→160 B，DescriptorIndex 376→400 B；
promise 40 B、Cpp Instance 128 B 不变。静态 shape 是只读数据，见大小/机器码原件。

12 个独立资源运行：P1/P4/P5 × 1000/10000 × 两侧。P4 10,000 实例 prepared 时：
heap busy 95,773,523→100,573,666 B；private commit 100,761,600→105,586,688 B。
主要可计算 backing 增量为 function binding 40×100,000=4,000,000 B、六步骤发布项 8×6×10,000=480,000 B、
Cpp Event admission 32×10,000=320,000 B，合计约 4.8 MB；其它小项/进程 heap 波动保留原始数。
Lua live 11,138,443 B、active pages 21,102,592 B、pinned 445,056 B、rounding 8,898,329 B、
heap alloc/free 328/5 两侧相同。正式计时未开这些统计。
同步任务 thread/lua_resume/root release 仍为零；最终 active frames/waits/leases 为零。
close 的资源观察在 backend owner 尚存时取样，池预留继续存在，不把它误报为遗漏释放或零内存。
没有完整闭合每个 STL/进程 heap 到七 owner 的总账，因此不宣称所有内存均已归因。

## 正确性与安装

- 21d10601 独立 clean clone、ValidateTrackedSnapshot，通过 Toolchain all 和 Developer all；均 `-j 4 -- -k 0`。
  两边第二轮为 no work。全量 CTest Toolchain **110/110**、Developer **127/127**；Lua55 固定 VM 合同 **4/4**。
- 现有 15 个加 native-task-lua-steps，共 **16 个** SDK 消费者重编译/运行；原 **13 类**值生成增量通过。
- 原四类组合增量 + 本次 native-step-shape，共 **5 类**坏输入拒绝、恢复重建/执行、no-op 通过。
  shape 只改 native 声明，Lua asset hash 不变；runtime 拒绝错 shape，恢复后原三场景都执行成功。
- **3 条**迁址链重新生成/编译/运行：script-lua-values、lua-script-packager、native-task-lua-steps；
  原受控 source/build/SDK 临时不可用，完成后均恢复。无旧 SDK/私有测试头 fallback。
- 新负例：缺失/错误 native shape 时 objects/provider/leases=0；flat void error/yield/engine await 时不新增等待、frame 一次释放；
  TBC 错误正确执行一次 close 业务；pcall recovery 后 provider=1；int32 在 Lua 仍是 float。
- 保留合法 owner=null/publication=0 的无状态 callback：completed=1、result=31、frame=1。
  真实返回错误、record OOM、转换重入/撤权、BeginPlay/EndPlay、身份复用、取消、容量晚接纳与共享协议原断言保持。

安装接口中增加正式 typed shape，不新增私有头依赖。修改的是 engine 公共头，不是触发三前缀同步规则的 modules 公共头；
独立最终 SDK 与 Toolchain 安装均重新生成。Android、其它工具链/平台未运行，不恢复旧 Lua VM。

## 失败、未选项和限制

opt3 编译曾因模板逗号落入 lua_pushcfunction 宏而失败，正式代码已用括号闭合；原失败日志保留。
初次切换 clean source 的旧 cache 拒绝、已有日志防覆盖拒绝和一次 TableGen 第二轮仍有工作均未写 PASS；
最终清理配置身份后两边第二轮 no work。部分重复 DevShell 启动报输入行过长，已有实际 compiler 命令和退出码另存；
最终 fresh shell 的新 shape 检查正常完成。新增增量原驱动清单漏列 shape，发现后用独立单项运行补齐，未拿未执行分支计 PASS。

未另试 closure/upvalue root、全局 authority cache、StepContext 重造、无证据的 move/symbol 优化或关闭公开统计。
StableSlotMap 结果 insert→find 保留；只复用 owner 内、无用户代码且引用稳定的 Event 新记录。
此前 T07 穷尽准备分配失败、T22 独立 main-stack OOM、T51 完整 owner 总账等限制仍以原报告为准，
本轮通过的具体负例不冒充这些范围已穷尽。internal Lua C 调试帧减少属于记录在案的可观察变化。

原始记录入口：[本轮归档清单](evidence/hotpath.json)、[原始 ZIP](evidence/hotpath.zip)。
归档已在提交 `841aae662a6d25975f4051559221119a986e229c` 推送，并从独立 bare 仓库重新取得，601 项文件哈希全部匹配。
[远端回取凭据](evidence/hotpath-remote-readback.json)记录固定下载入口；文档提交不会改写资格 source。完成后等待独立审阅。
