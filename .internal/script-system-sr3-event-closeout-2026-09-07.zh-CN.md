# SR-3 持续 FlowForge Event 有限调查与交付（2026-09-07）

**结论：两项有限假设未能解释残余，未保留新的生产运行时修改；持续 Event 成本未收口。**
保留 `99c1d095` 的紧凑 InvocationState、调用票据、相邻身份查验复用及全部安全修正。
新增真实 single-flight/代次复用负例和计时区间外的完整性观察。两项实验及无效试验均保留原始记录。
不以 Update 改善、绝对差较小或试验无收益为理由，宣称残余是可接受或必要安全成本。停止于 SR-3。

## 1. 身份与实际差异

- 接手时本地/远端 HEAD：`96305a939d16e650fac930926f8c1d6266ec0447`。
- B0 历史参照：`8145598c18421d03da1dac21251200af3290e27d`。
- B2 生产参照：`99c1d095fad6a728c3d808ff2d853ce84b59bfea`。
- B3 **验证源码快照**：`8a6e6ef468f7eb60265678ab25116719b42a01a6`，其运行时 src/pinclude 与 B2 无差异。
  后续提交只有报告、原始证据和索引；不以这些 SHA 重标 EXE。
- lux-cxx：`3100f54d0743c5ed94a4ccf5943df04e933de255`，114 个安装头重新按 CRLF→LF 比较全部匹配。
  Toolset 固定 `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`，未升级依赖。
- 保留未知测量脚本修改、两个未跟踪文件及 main 五项修改。原 sr2/sr3-gate/sr3/sr3-cost 证据 blob 未改变。

B3 实际提交涉及 continuation fixture、Flow benchmark、匹配安装消费者构建脚本和测量/汇总驱动。
没有新增生产 ABI、公开统计、调度机会或恢复预算，没有单侧 LTO。私有组件不安装。
完整源码、driver、安装头、生成器、DLL/EXE 身份见[新证据索引](evidence/script/sr3-event/README.md)。

## 2. 实际工作账目与优化产物

独立私有源码副本在已有统计取样处读数：60 次 warmup 后至第 360 次 occurrence，差值覆盖 300 帧。
每个替换检查准确命中数；空输出/缺失探针直接失败。最终 B2/B3 使用同一 observer 源码。
探针开销和输出不进入正式性能结果；正式每 handler 不读时钟。

| 300 帧实际计数 | B2 / 最终 B3 |
|---|---:|
| Hook 候选 / 普通 Event callback 候选 | 3,000,000 / 0 |
| 身份或方法拒绝 | 0 |
| 建立 Invocation / single-flight 跳过 | 3,000,000 / 2,400,000 |
| 实际新 step / sync / backend resume | 600,000 / 0 / 600,000 |
| 新等待 / claim / copy / 终态提交 / queue push | 各 600,000 |
| pop / stale pop | 600,000 / 0 |
| resume 完成 / 再挂起 / 失败 | 600,000 / 0 / 0 |
| continuation destroy / waiter unlink | 各 600,000 |
| copy 拒绝 / ACTIVE waiter 取消 / recordFailure 调用 | 各 0 |
| 稳态 queue / continuation / awaitable / waiter | 8,000 / 8,000 / 8,000 / 0 |

这是 Hook 启动、等待 Event、预算恢复的持续场景，不能称为每帧 10,000 次 backend 调用。
测量前后 backlog 差值为零，但 backlog 存量为 8,000。关闭另毁最后 8,000 个 continuation，
不新增 provider 工作；累计 728,000 次启动/销毁、720,000 次 resume，最终各执行资源与保护计数为零。
provider calls/checksum 在关闭前后不变。正式 3,000 帧的实际分母为 6,000,000 次 resume，Hook 候选 30,000,000。

**主假设支持到“操作存在”，没有支持到“主要根因”。** B2 实际优化 OBJ 的 State::invoke 在 offset `0xFE`
执行 `inc qword ptr [rdi+1F8h]`，`rdi=State+0x208`，先于 active_hook/single-flight；跳过出口仍有减计数。
编译器未消除这对写入。试验在合法索引/完整身份/ACTIVE 查验之后、无用户代码的连续窗口判断 single-flight，
真实调用再取得原保护。探针证明票据由 3,000,000 降至 600,000，业务、等待、恢复和 backlog 不变。
但 MSVC 将新模板 invokeAccess 留作每候选的额外调用边界，State::invoke 栈由 `0xE0` 变成 `0x100`；
它不是只删除两条写入的等成本对照，不能把净差全部归因于计数。未继续无依据的 forceinline 搜索。

**次假设：两层保护确实重叠，但合并净收益未获支持。** resumeOne 的 `0x16E` 写 `[r15+1F8h]`，
其中 `r15=State+0x208`；`0x1DE` 写 `[r14+400h]`，其中 `r14=State`，均为同一保护计数。
外层 Invocation 覆盖 backend、返回重验和失败清理，内层 UserInvocationScope 只包 backend。
当前计数的消费者判断是否非零，未发现按具体层数区分的语义。仅删除内层的独立试验保留外层和其他入口，
窄回归通过，但没有可重复收益，已撤销。没有将一个 guard backport 冒充完整安全等价；本轮未扩展 B0-safe 实验。

| 有效但撤销的试验 | 五对 %，按 pair 顺序 | 中位 % |
|---|---|---:|
| 跳过前免票据，原 300 帧 | +2.33, -18.11, -1.60, +0.84, -1.84 | -1.60 |
| 同一补丁，固定 3,000 帧 | -2.27, +1.05, +2.03, -3.80, +0.97 | +0.97 |
| 单独合并 resume 重叠保护，3,000 帧 | -3.60, +1.69, -0.99, -0.21, -0.51 | -0.51 |

未剔除离群对。第一次探针误用源文件路径导致无 PATH 输出，被严格拒绝；第一次负例编译因 expected<optional>
解引用错误失败，修正并全量构建后才运行。原失败日志保留，两者不作有效成本证据。早期迭代 EXE stamp 可能旧，
用实际 DLL SHA+patch 识别；不作为最终 clean qualification。有效试验无收益与无效试验分开。

## 3. 保护责任与真实回归

最终没有调整任何生产保护窗口。Instances 仍独占完整身份、方法范围、ACTIVE 权威及票据；执行区独占 active_hooks；
Bindings 独占 handler/token。真实 invoke 的票据覆盖原结果处理；resume 的两层保护保留。
用户代码返回后 current() 重读动态状态；sameIncarnation 仅用于原同步错误归属，不恢复调用或等待资格。
prepared Ability 直达 provider、Event 多飞、实例/方法/代次隔离及所有独立 copy/destroy/lease/rollback 保护不变。

| 旧断言/本次补充 | 实际入口与核对内容 |
|---|---|
| 新 single-flight 负例 | continuation fixture 的 testSingleFlightIsolation：重复 16 次只建 1 frame/Awaitable；同实例第二方法和另一实例可调用；budget=1 只恢复一次；精确销毁 4 次 |
| 新代次复用负例 | 同函数第二段：实例 slot 复用而 generation 改变；旧 completion 拒绝，新 Hook 正常启动；两次 backend/continuation 最终销毁 |
| 七点合法重入/同步错误 | lifecycle testProtectedReentry：create、prepare、invoke、continuation destroy、releaseMethod、backend destroy、lease release；额外同步 status 32 的原 mount INVOCATION_FAILURE；8 条 REENTRY_OK 精确命中 |
| 越界/旧身份/move-pin、Bindings | testInvocationAuthority 与 BINDINGS_OK，原 token busy、部分发布/连接回滚断言保留 |
| 完成/再挂起/容量/取消 | testSyncAndContinuation、testAsyncAbilityInvocation、testCapacityAndCancellation，执行真实 fixture |
| occurrence、多飞、预算、写入保护 | event fixture 的 testBroadcastSemantics、testRegistrationCutoff、testNestedDispatch、testResumeBudget、testCopyRetirementPin、testCopyOtherRecordRemoval、testCopyShutdownAndFailure、testCopyNestedAdmission |
| frontier/装配/错误回收 | ingress fixture、lifecycle testResolvedBatchProtocol、testResolvedInputExpiryAndReuse、testResolvedAssetFailure、testResolvedPreparationFailures；原断言未迁移到假入口 |

独立 clean clone `lux-engine-sr3-event@8a6e6ef4`，`sr3-e/{t,d}` 构建/安装；仅 RelWithDebInfo，
`all -j 4 -- -k 0`，构建与测试/测量串行，两 profile 第二轮均无工作。相关 fixture 明确 `/UNDEBUG`。
Toolchain **107/107**；Developer **113/119**。14/14 安装消费者通过，6 生命周期+8 Event 严格轨迹与 288-byte wire golden 匹配。
直接 runtime 实际链接无 World/Scene/Process；描述 leaf 独立消费通过，私有头未安装，26 export 名称相同（不等于完整 ABI 证明）。

六项失败在 B0/B2 本轮重跑及 B3 中均为同一 `activeContinuationCount() == 0U` 断言，CTest exit=8：
`scene_script_lua_runtime_test`、`scene_script_lua_runtime_workers_0`、`scene_script_lua_runtime_workers_2`、
`scene_script_lua_runtime_workers_4`、`scene_script_lua_runtime_interpreter_test`、`scene_script_lua_runtime_interpreter_workers_4`。
逐条名字、断言原文和退出码见 diagnostics/verification.json；没有新增失败，不称全绿，不扩展修复历史 Lua。

## 4. 最终无探针同量配对

以下 B3 是无新运行时补丁的验证快照。双方 Flow EXE 从同一 benchmark 源码构建，链接各自固定安装产物，
嵌入真实 runtime SHA；完整源/EXE/DLL 哈希在 observer identity。B2 副本有 20 行换行符差异，CRLF→LF 后三方源码逐字节相同；
原字节哈希保留，observer-verification.json 记录此次核对。Flow 的 INTEGRITY 只在全部计时后及 shutdown 后打印。
每场景五对独立进程，平衡先后顺序；另有独立分配诊断。Update/Flow Event 3,000 帧，Lua 300 帧，fan-out 6 行。
固定 size=10,000、warmup=60、seed=1592598566，Flow Event budget=2,000，affinity=1，高性能电源方案。
两组比较分别顺序运行；频率和后台负载未锁定，不把帧当独立样本，不拼乘百分比，不对候选单开 LTO。

| 场景 | B2→B3 中位 % [min,max] | B0→B3 |
|---|---:|---:|
| cpp-update | -0.87 [-5.65,-0.32] | -9.67 [-11.35,-5.16] |
| event-fanout | -9.10 [-13.08,+9.67] | +3.57 [-11.60,+26.49] |
| flow-event | +0.30 [-0.50,+0.90] | +13.72 [+9.99,+15.45] |
| flow-update | -0.60 [-2.48,+7.14] | -3.11 [-12.21,+2.42] |
| lua-event | +0.10 [-3.99,+1.44] | +6.66 [+5.03,+8.42] |
| lua-update | -2.52 [-7.30,+3.97] | +1.74 [-5.72,+10.16] |

| 对照 / 场景 | 基线/候选批量中位 ms | 实际操作数 | 基线/候选 ns/操作 | 配对差中位 ms |
|---|---:|---:|---:|---:|
| b2-b3 / cpp-update | 143.1775 / 141.7822 | 30,000,000 script_calls | 4.773 / 4.726 | -1.3250 |
| b2-b3 / event-fanout | 2.2327 / 2.2507 | 10,000 resumes | 223.270 / 225.070 | -0.2204 |
| b2-b3 / flow-event | 2998.8808 / 3011.0577 | 6,000,000 resumes | 499.813 / 501.843 | +8.9460 |
| b2-b3 / flow-update | 282.8315 / 282.2837 | 30,000,000 script_calls | 9.428 / 9.409 | -1.6989 |
| b2-b3 / lua-event | 1987.3292 / 1983.1438 | 3,000,000 resumes | 662.443 / 661.048 | +1.9887 |
| b2-b3 / lua-update | 79.6915 / 79.5287 | 3,000,000 script_calls | 26.564 / 26.510 | -2.0557 |
| b0-b3 / cpp-update | 159.1496 / 145.6098 | 30,000,000 script_calls | 5.305 / 4.854 | -15.4492 |
| b0-b3 / event-fanout | 2.4320 / 2.5263 | 10,000 resumes | 243.200 / 252.630 | +0.0991 |
| b0-b3 / flow-event | 2627.0361 / 2978.5973 | 6,000,000 resumes | 437.839 / 496.433 | +360.4816 |
| b0-b3 / flow-update | 292.7962 / 282.2136 | 30,000,000 script_calls | 9.760 / 9.407 | -9.1091 |
| b0-b3 / lua-event | 1878.0652 / 1993.4117 | 3,000,000 resumes | 626.022 / 664.471 | +123.6480 |
| b0-b3 / lua-update | 77.6303 / 77.5108 | 3,000,000 script_calls | 25.877 / 25.837 | +1.3516 |

| 冷期 | prepare ns | 7 项 late ns | 128 次 remount ns |
|---|---:|---:|---:|
| b0-b3 | 122,500→136,500 (+10.24%) | 7,000→11,000 (+55.71%) | 80,600→101,900 (+27.71%) |
| b2-b3 | 134,700→136,400 (+2.67%) | 11,000→11,400 (+5.36%) | 103,100→103,000 (-0.77%) |

| 128 次重建 | 配置/endpoint 数 | 基线/候选 ns | 配对中位 % [min,max] |
|---|---:|---:|---:|
| b0-b3 | 8 | 75,200 / 95,300 | +24.16 [+20.74,+31.76] |
| b0-b3 | 8,192 | 354,900 / 96,900 | -70.75 [-75.80,-67.86] |
| b2-b3 | 8 | 92,200 / 91,800 | -0.43 [-1.50,+0.99] |
| b2-b3 | 8,192 | 94,100 / 95,700 | +1.71 [-3.60,+2.66] |


所有配对批量总时间、逐对绝对差/百分比及实际操作数见[一页 CSV](evidence/script/sr3-event/one-page.csv)与
[JSON](evidence/script/sr3-event/one-page.json)。表中 ns/resume 是完整帧工作摊销，不是孤立 resume 函数延迟。
Flow update 的 provider 工作为两倍脚本调用；Flow Event 以真实 provider/resume 增量核对，旧 completed 字段不重标。

本次 Flow Event B2→B3 配对中位 +0.30%，五对范围 -0.50～+0.90%；B0→B3 为 +13.72%，范围 +9.99～+15.45%。后者配对总时间差中位 +360.4816 ms/3,000 帧，即完整工作摊销 +60.080 ns/resume、+0.120161 ms/本例帧。即使 B2/B3 源码相同仍存在进程配对波动，不把这部分写成新补丁收益；也不把历史 B0 的相同业务工作当作完全相同的安全语义。

冷期每对业务 creates/destroys=152、prepares/releases=456。8/8,192 配置均固定只重建一个实例，16 warmup+128 次计时，
两种规模最终 creates=destroys=configs+144、ticks=configs、errors/backlog=0。B2/B3 两种规模均只访问 128 个 slot、
0 个 endpoint 计数项；128 次完整重建循环内的分配诊断均记录 530 次 EXE-local new，含 fixture 可观察分配，原始记录保留。
这些窄回归只记录，不为其残余新增优化项目。B2/B3 均保留 Mount 360 + InvocationState 40 bytes/config，
相对 B1 原 384 bytes 合计多 16 bytes/config（10,000 配置 +160,000 bytes），另有 vector/backing 分配，未含 allocator 元数据。
尺寸沿用已审阅编译器布局报告，并以本轮运行时源码相同核对；没有将旧布局输出重标为新测量。
本轮没有新生产存储；不得把热数据紧凑称为总存储减少。EXE-local new/Lua VM 计数不能代表全部 DLL 堆分配。
补充的匹配源码 Flow 分配诊断在 Update/Event 两侧三帧均为 0 次 EXE-local new。初次联合驱动使用各历史 fixture 源码，
其分配观察保留但不作为最终匹配对照；最终以 diagnostics/matched-flow-allocations 的相同源码重跑替代。

旧 CSV 的 errors/failures 仍为 null，不改历史记录。匹配 Flow observer 的两次保留错误记录均为 0，关闭后执行资源为 0，
调用/checksum 不变；这是保留记录而非新的累计错误 API。当前源码没有在该运行中清 failures，容量 16，
私有同输入探针额外观察 recordFailure 调用为 0；该窄诊断不能替代所有场景总错误数。
没有新硬件周期/cache-miss 数据；本轮依靠真实分支频数、优化机器码、有限受控补丁和进程配对，未另建 profiler。

## 5. 审阅决定与停止

建议保留 B2 运行时和本次测试/观察工具，拒绝两项无稳定收益的复杂度。已证明跳过路径的写入存在，也证明
重叠保护存在；尚未隔离封装边界、数据访问、保护及其机器码交互各自对最终残余的份额。
本轮的准确交付属于“有限假设未能解释残余”，不是性能等价或必要安全成本证明。
用户/审阅方可依据本次绝对影响与完整分布决定是否接受 Event 性能债务；本报告不替用户接受，
也不自行开启全路径微优化或 SR-4。原始试验和撤销补丁可复核，无需重建旧证据上传流程。

阶段日志：核对固定身份 → 原规模实际计数/产物 → 主试验及长批配对 → 次试验 → 撤销 →
新增负例与匹配观察驱动 → clean qualification/安装/窄回归 → 新证据增量与提交推送。完成后停止，等待独立审阅。
