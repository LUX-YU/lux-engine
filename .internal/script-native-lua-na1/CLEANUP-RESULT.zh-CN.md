# 合并前脚本清理与有限优化结果

正确性修正和本轮有限清理已实施，测试范围内通过。**成本为混合结果，尚不能标为性能收口，也不代表批准合并。** 32 次 Event 长任务和 FlowForge Event 各有约 2% 的新增小回退，原样登记；未用其他场景收益抵消。

## 身份与入口

- 修前生产及封存 E 镜像：`841320a36e864145eda43fd6b775a10a696d8e7f`；任务入口审计提交 `7c5b9bcb`。
- 最终实际资格源码：`e39535cfa9d2ebbcdeaeb165c00c7773d52db7cc`，独立 clean clone、F/F-common/F-flow 镜像。后续报告/归档提交不是新测试身份。
- 固定 lux-cxx `3100f54d`、toolset `99c3d048`；实际安装的 204 项依赖文件重新校验。Lua 5.5.1 原 DLL 哈希、VM patch、INC、16 MiB、Native ABI6、MSVC 19.44/RelWithDebInfo 均保持。
- main 的 7 项未知修改逐项哈希一致。仅在 `codex/s6-native-task-lua-steps` 提交，不合并 main。
- [修前审计和负例](PREMERGE-AUDIT.zh-CN.md)、[本轮逐项结果](../evidence/script/premerge-cleanup-20260912/findings.csv)、[机器可读结果](cleanup-result.json)、[原始归档入口](../evidence/script/premerge-cleanup-20260912/INDEX.md)。

## 实际修改

1. **F01：普通同步 Lua 参数转换。** 准备期将可能执行 converter 的调用选到受保护入口，在转换前捕获原资格，转换后、Lua 函数体前重验。保留 standalone、生命周期和延迟 stop；小 scalar 入口不增加该协议。外层 ExecutionScope 的拥有型状态位于 pcall 外，C callback 只借用对象；typed converter 返回失败，不允许 longjmp 越过其拥有型 C++ 局部对象。
2. **F02/F03：边界正确性。** Simulation/real delay 共用排他 2^63 上界检查，先 ceil 后验证、再 cast；保留原错误优先级。小缓冲扩缩容只有 spill→inline 才复制，移除 inline 的同地址 memcpy。
3. **F04/F05：死代码和释放。** 删除无读者的 resume_enqueued、未调用的 awaitableId 和 Lua scalar reader。Ticket 与 Allocation 各自完成必要外部验证后，共用一个 releaseResolved；不再构造 Allocation 重新验证自己。
4. **F06/F07/F08：短区间复用。** 内部 optional 类型描述改为只读借用；结果槽直接清理 bytes/type，减少临时拥有型结果。Native 准备好的 step 布局不在创建 continuation 时重复验证。Timer 完成前只定位一次，回调后仍按 ID 取消，防止使用已移动/释放的记录。
5. **F09/F10/F11：反馈与工具。** 反馈改有界环形队列，部分 collect 不再搬动所有剩余元素；仍按首次变脏顺序确认。INTEGRITY 中的 failures 数改名 retained_failures，不再冒充空页内存。旧成本 fixture 删除 LuaRecordMarshaller 分支，默认使用当前生成 codec；不同职责的 ProtectedRecord 诊断保留。
6. **F12：检查工具的旧缺口。** 样式脚本现在解析相对路径并拒绝空扫描；原先 `-DLUX_SOURCE_DIR=.` 可能假通过。未扩展到全仓库格式重写。

没有新的 scheduler、结果池、公共 trusted 参数、兼容别名或语言功能。身份/容量/取消/写 pin/事件 cutoff/恢复预算和逻辑槽归还时点保持。

## 正确性与安装

[真实 oracle 输出](../evidence/script/premerge-cleanup-20260912/final-oracles.log)包含：

- 转换保持有效：body=1/provider=1；实际退休：body=0/provider=0；只请求 stop：body=1/provider=1；转换返回失败：body=0/provider=0。关闭后 prepared 槽释放，无新增任务 Lua thread。
- 本地和通用 Timer 各检查边界相邻三个 double；越界 waits=0/errors=1。1.1 ns 在 1 ns 后不恢复、2 ns 后恰好恢复一次；elapsed 叠加溢出被拒绝。real delay 合法输入 endpoint=1，越界 endpoint=0，关闭后旧 completion 失效。
- 内联/溢出存储、移动、自移动、错 alignment；票据错 owner/page/slot、旧 generation、重复释放、外部 data/size 不匹配；反馈绕回和重新变脏顺序 `2,3,1`。

最终 Developer **131/131**、Toolchain **110/110**，两套全量 `all -j 4 -- -k 0` 和第二轮无工作；原 VM 合同 **4/4**。当前安装消费者 **16/16**，13 类值生成增量、5 类步骤/任务/路由负例、3 条迁址链通过；源码/构建/原 SDK 在迁址时不可用，结束均恢复。额外的已迁移成本 fixture 编译执行和 protected-errors 通过。

保留全部失败尝试：初始测试行宽失败；错误地假设登记失败也创建 continuation 的用例；不符合 typed converter 合同的直接 luaL_error 注入；额外诊断首次遗漏 compile_commands 导出。修正测试/驱动后通过，不能把这些初次运行记为 PASS。串联驱动多次进入 VS 环境出现命令行长度警告，实际编译路径、产物和执行已核对；不是生产测试回退。

## 同量成本

九个原完整场景加四个受影响语言控制组；各三对 AB/BA/AB，**78 个有效进程，未丢弃慢组**。固定 seed、affinity=16、预算、资产、warmup；统计/采样另跑。两边逐行业务及最终计数匹配，观测到的错误和 ready backlog 为零。[全部批量时间、实际操作数、p50/p95/p99 和配对](../evidence/script/premerge-cleanup-20260912/performance-runs.json)。

下表 A/B 是三个进程均值的中位数；变化列是三个配对变化的中位数，二者不能直接相除冒充同一个统计量。

| 场景 | A | B | 配对中位变化 | 三对变化 | 单位 |
|---|---:|---:|---:|---|---|
| 原生任务＋Lua：单 Event | 332.210 | 326.185 | -2.19% | -2.19% / -3.20% / +1.78% | ns/完整调用或任务 |
| 原生任务＋Lua：NextStep | 497.434 | 461.070 | -7.74% | -7.74% / -11.39% / -7.31% | ns/完整调用或任务 |
| 原生任务＋Lua：三等待 | 1130.928 | 1086.949 | -2.34% | -4.44% / -2.34% / -1.59% | ns/完整调用或任务 |
| 原生任务＋Lua：32 次 Event | 5901.577 | 5993.042 | +1.57% | +1.57% / +2.10% / +1.55% | ns/完整调用或任务 |
| 原生任务＋Lua：ValuePose | 1318.069 | 1298.880 | -1.46% | -1.46% / -1.58% / -0.64% | ns/完整调用或任务 |
| 原 Lua scalar Ability | 121.636 | 120.083 | -1.28% | -1.50% / -1.28% / -0.73% | ns/完整调用或任务 |
| C++ Sequence | 1.873 | 1.881 | +0.06% | +2.81% / +0.06% / -0.53% | ms/原帧 |
| FlowForge Event | 182.308 | 185.651 | +1.76% | +1.61% / +1.83% / +1.76% | ns/完整调用或任务 |
| 混合 Physics | 1.944 | 1.963 | +1.05% | -0.17% / +1.10% / +1.05% | ms/原帧 |
| 原 Lua Event | 516.934 | 510.851 | -1.73% | -1.89% / -1.73% / -0.84% | ns/完整调用或任务 |
| 原 Lua NextStep | 621.468 | 617.400 | -1.87% | -0.65% / -1.87% / -2.59% | ns/完整调用或任务 |
| C++ Update | 3.504 | 3.349 | -4.43% | -1.19% / -4.43% / -6.61% | ns/完整调用或任务 |
| FlowForge Update | 7.569 | 7.220 | -4.61% | -4.61% / +2.22% / -17.82% | ns/完整调用或任务 |

P4 新增约 **91.47 ns/完整 32 等待任务**（三对都慢）；这只是完整任务差异，不能分摊后声称已测到单次 resume 的成本。FlowForge Event 新增约 **3.34 ns/任务**，三对都慢。Physics 为混合场景，约 +0.019 ms/帧，不能归因到单一 backend。C++ Sequence 波动跨正负；FlowForge Update 三对离散很大，不能把中位数当稳定保证。上述差异未被认定为必要安全成本。

## 机器码、资源与热点

[机器码摘录](../evidence/script/premerge-cleanup-20260912/machine-excerpts.asm)：Ticket 入口静态指令 51→38，调用目标由重新 checked 的 Allocation 入口变为释放内核；它仍有一次真实调用。结果槽释放本体的 call 指令 2→1。admitAwaitable 本体仍为 151 条字节指令、9 个 call，不能仅凭 const 引用就宣称它本体变快；变化主要是调用方传递/临时对象，最终以完整业务为准。未全局 forceinline 或单侧 LTO。

AwaitableRecord 仍为 **240 B**；ScriptInstances 从 **576 B→592 B**，增加两个反馈游标。[六组配对资源观察](../evidence/script/premerge-cleanup-20260912/resource-summary.json)的 VM、frame 和 owned backing 计数相同；1000/10000 实例、P1/P4/P5 均保持释放闭环。不把瞬时 RSS/heap walk 差值说成确定内存收益。

新 P4 精确业务 ROI 软件 Hotspots 自耗时合计 3.636 s：waitEvent 9.90%、Lua 字符串键查找 6.85%、resumeOne 6.50%、结果槽释放 5.30%、C++ wait 3.73%。[采样摘要](../evidence/script/premerge-cleanup-20260912/profile-summary.json)及原始 result/导出在归档中。函数内联/归属变化会改变 self time；这次采样不能精确解释约 2% 的正式计时差异，也没有 PMU 证据支持 cache miss/分支预测归因。

## 状态与限制

- 实现：正确性缺口修正，有限清理完成；F08 的回调后取消检查明确保留。
- 合同：指定 Windows RelWithDebInfo 范围通过；未运行 Android、旧 Lua VM 或未授权环境。
- 成本：**混合，Event 小回退未解释，未完全收口**；不称等价、不用历史债务豁免新增差异，也不继续无边界微优化。
- 原 scalar/coroutine/Native metadata 等历史债务继续引用 [既有记录](LOCAL-WAIT-RESULT.zh-CN.md)，未重标旧源码或旧测试身份。
- 合并/正式采用：`NOT_APPROVED_PENDING_REVIEW`。不合并 main、不发布 tag、不开始新架构阶段。
