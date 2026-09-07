# SR-3 热路径成本收口（2026-09-07）

## 1. 身份与范围

B0 为修正 SR-2 `8145598c18421d03da1dac21251200af3290e27d`；B1 为已审阅 SR-3
`3e2bcbd29580d2e744744b64149b7d8fedcc8950`。开工本地/远端为 `4297022c97bfefa6aa53af942d429687ee19228e`。
lux-cxx 固定 `3100f54d0743c5ed94a4ccf5943df04e933de255`、`install/q2/c`。
未知测量脚本修改、两个未跟踪文件和 main 工作区不纳入提交。既有四个证据包不改写。

本轮仅 SR-3 私有实现成本修正，不进入 SR-4/5/6；不改变 backend ABI、调度、预算、组件所有权。
生产候选 B2 为 `99c1d095fad6a728c3d808ff2d853ce84b59bfea`，包括前置窄改 `9406f72c380cec6e119e50499f392db38d92eaca`。
`9406` 的独立资格和性能单独保留为中间候选 C2，不冒充 B2。文档/证据提交不改变上述生产代码身份。

## 2. 实际调查与保留改动

在相同已资格构建上重新跑 B0/B1 六场景五对进程，完整 CSV 业务输出相同，C++/FlowForge 回退仍存在。
MSVC `/O2 /Ob1 /Zi /DNDEBUG` 的 OBJ 反汇编及同参数编译报告表明 B1 `invoke` 每 handler 有实际调用，
同步路径承担可恢复分支的栈保存；`Invocation::current` 也可能产生实际调用。B0 的 Hook 遍历内有内联调用代码。
每 endpoint 的 port 和 Traversal 开销不能按 handler 数相乘；B1 已无每 handler 的函数指针转发。

WPR CPU 采样受系统策略拒绝 `0xc5585011`。使用独立诊断进程的用户态主线程 RIP 采样，保留地址、模块和
PDB 解析结果；包含启动/关闭和采样扰动，不作为正式耗时，也不提供硬件 cache miss 或周期指标。
最终 B2 的 C++/FlowForge update/Event 样本分别为 1,119/1,855/1,956，进程均成功退出。
C++ 有 472 个样本归属 invoke、106 个 HookLane；FlowForge update 有 472/110 个。
Event 的 key/身份路径 172、invoke 77、waitEvent 76、eraseEventWaiter 73、createAwaitableRecord 64。
这些是 PDB 归属的抽样点，包含内联位置，不是函数调用计数；不同采样总数也不能直接当性能百分比。

撤销的实验：单独拆出 resumable 分支、强制 invoke 内联、再强制访问者内联，都改变了机器码边界，
但计时没有稳定证明收益。只有字段重新排序仍留下整个 Mount 的大步长。无永久 FAST/LEGACY 开关。

保留的窄改：

1. `ScriptInstances` 内新增固定容量 `InvocationState` 数组，唯一拥有 method 范围、当前/退休完整实例 ID、
   生命周期状态。冷 Mount 只持有对应项的内部指针，不保存这些字段的第二份值。所有原写操作完整迁到同一权威。
   `prepare` 一次分配两个完整 backing 并关联；发布后两数组不增长。统计包含两份 backing 的总字节。
2. `Invocation` 自身表示空/有效票据，不再包一层 `optional`；owner 指针为空即无资格。有效票据只借用
   稳定的 const prepared 和 const InvocationState 地址，消除重复索引/地址计算和 optional 中间搬运。
   票据不可复制、移动转移一次保护；析构只撤销它取得的一次 pin。
3. `findActiveSlot` 在 owner 内一次完成完整代次及 ACTIVE 查验。`resumeAccess` 和 `eventSource` 复用该次
   查验所得槽位，不在没有分配/用户代码的相邻语句再次执行同一个 SlotMap 查找。`eventSource` 定义移到
   私有头，保持值快照和全部 admission/scope 校验；不是公开头、宏开关或通用强制内联策略。
4. `beginSuspension` 接收调用者刚在 backend 返回后重新验证的票据身份。原来的整个冷 Mount 值快照只用到
   instance 字段；现在传入相同的完整 ID。票据尚未析构，期间没有新的用户代码，后续 destroy/failure/attach
   仍归属该身份，不提前结束保护或扩大其权限。

紧凑数组的对照和撤去新票据的消融实验分别保留。最终接受依据是正式 B0/B1/B2 同量配对，不是源码 inline 数量。

最终同参数 MSVC layout 报告：B1 Mount=384 bytes；B2 冷 Mount=360、InvocationState=40、Invocation=32。
hot 字段 offset 为 method_first=0、method_count=8、instance=16、retiring_instance=24、state=32；
相对 B1 backing 总量增加 **16 bytes/config**，另有一个 vector 对象和一次独立 backing 分配。
10,000 配置增加 160,000 bytes，8,192 配置增加 131,072 bytes（不含 allocator 元数据），并非内存净减少。
`mount_backing_bytes` 统计明确包含两份 backing。缩小的是每次准入读取的记录步长；prepared 条目仍为原路径。

### 2.1 归因范围与撤销实验

| 观察对象 | 实际证据与归因边界 |
|---|---|
| endpoint port / `visitHook` / `Traversal` | 每次 endpoint 交付进入一次；深度增加和结束处理在 handler 循环外。不能把它乘以 10,000 当作每 handler 间接调用。Bindings 源码本轮没有修改。 |
| `State::invoke` / ticket / `current` | B1 的实码存在每 handler 调用边界、票据构造与重新索引；对象代码、最终 DLL 和 PDB RIP 采样都保留。可恢复分支拆出确实缩小同步函数栈，但单独计时未证明稳定收益，因此撤销。 |
| authority 字段 | B1 的 method 范围、身份与状态跨越大 Mount 的多个缓存行。紧凑数组试验与撤去新票据的消融能区分“只移动函数”和“改变每次准入读取路径”。未取得硬件 cache miss 计数，不把该证据写成精确缓存缺失归因。 |
| Event 身份查找 | B1/C2 的 `eventSource` 实码有两套相同 SlotMap 验证序列；替换为一次后长测中位改善约 1.5%。身份检查本身未删除。 |
| Event 值快照/调用边界 | 私有定义可见后，实码从调用 `eventSource` 变成调用 `findActiveSlot` 后直接执行其余验证；增量收益与噪声不能精确分开。组合再去掉 suspension 冷快照后五对改善 2.60%–8.50%，中位 4.92%，不是把每项收益简单相加。 |

Event 定位用 3,000 帧、每帧 2,000 次实际 resume/provider 调用、固定 8,000 backlog，比最初 300 帧更长。
各实验只替换诊断运行目录中的 ScriptSystem DLL，EXE、DLL SHA 和源代码 patch 同时记录；这些诊断不是安装资格。
最后 B2 从独立 clone 重建全部 DLL 和 EXE，再进行正式配对。

撤销的 `split`、`boundary`、`combined`、`visitor` 和 `compact-only` 都保留 patch/构建日志/计时或反汇编。
未保留 `forceinline`，未扩大预算，未改变工作量，也未为任何一侧单独启用 LTO/PGO。剩余未解释 Event 成本
不能自动归为“必要安全成本”；本轮没有证明去除安全仍能等价的替代方案。

## 3. 不变的证明义务

- 调用前仍检查槽位范围、ID 非空及完整代次、ACTIVE、方法属于该实例的范围；未取消任何动态准入检查。
- `current()` 每次重新读权威状态/完整身份，不跨用户代码缓存 ACTIVE。`sameIncarnation()` 只匹配当前或
  退休身份以归属已返回错误，不发放准入资格。同步自行退休后的 status 32 仍记录。
- 地址稳定来源是完整容量预分配；身份有效来源是每次动态查验，两者独立。pin 覆盖调用、用户重入、返回处理，
  冷 Mount、backend、prepared、lease 的物理回收仍等待原保护窗口结束。
- Construction、失效、故障、退休、reset、status、反馈均在 Instances 中写同一 InvocationState。
  Bindings/Preparer/System 不取得可写字段。端点 token、发布与遍历保护完全不变。
- resumeOne 的独立 UserInvocationScope 保留；本轮不把它当作同步路径根因，也不改写 destroy/copy 的保护期。

## 4. 测试追踪

旧业务断言及入口保持原样：lifecycle 七点重入/同步错误/资产失败与混合批次，Bindings 回滚/token/130 回调，
continuation、Event、pin、frontier、预算、SR-2 装配和身份复用。

在现有 `simulation_script_bindings_test` 中增加 owner 层 `testInvocationAuthority`，使用真实 Instances 的
reserve/commit/construction/activate/revoke/retirement 操作，断言槽位/方法越界、无效/旧代次拒绝、移动后
只有一个 pin、退休后 current=false 而 sameIncarnation=true、旧身份不得恢复、回收后槽位新代次、prepared
地址复用。它不调用 backend；真实 backend 保护由原 lifecycle fixture 验证，不用辅助函数生成预期答案。

## 5. 测量口径与重放

同一台 i7-13700KF、Windows/MSVC 14.44、High Performance 电源计划。正式稳态配对统一继承 affinity mask 1，
没有锁 CPU 频率或关闭所有桌面后台负载；独立进程 pair 才是统计单位，帧不是独立重复。
两侧均 `/O2 /Ob1 /Zi /DNDEBUG` 的 RelWithDebInfo；测试通过 `/UNDEBUG` 保留业务断言。只有对称的既有
安装 IPO consumer 使用它自己的编译选项，不能把该 consumer 与普通 benchmark 混作性能参照。

`RunScriptSR2Measurements.py --short-path-frames 3000 --affinity-mask 1`，size=10,000、warmup=60、
seed=1592598566、worker=0；同一 VM/FlowForge backend。C++/FlowForge update 各 3,000 帧，
其他稳态场景 300 帧；fan-out 为一次 occurrence + 五次 drain。每场景五对 performance 进程，另外一对
diagnostic 进程。正常 resume budget=2,000；Lua Event 的实际预算为 10,000，双方一致并单独记录。

| 场景 | 单进程计时区间的有效工作 | 归一化与终态 |
|---|---|---|
| C++ update | 30,000,000 script calls、3,000 Hook occurrences | 总 ns / 30,000,000；backlog 0 |
| FlowForge update | 30,000,000 script calls、60,000,000 provider calls | 分别除实际 script/provider 次数；backlog 0 |
| Lua update | 3,000,000 script calls | 累积计数扣掉 warmup；backlog 0 |
| FlowForge Event | 600,000 resumes/provider calls、300 occurrences、3,000,000 Hook callback attempts | 总 ns / 600,000；backlog 8,000，每帧保持；legacy completed 列为序列专用，未冒充实际完成数 |
| Lua Event | 3,000,000 resumes、300 occurrences | 总 ns / 3,000,000；backlog 0 |
| Event fan-out | 1 occurrence、10,000 resumes、5 次 drain | 报告整个 6 行批次总时间及每 resume；初始 waiter 登记不在计时区间内；backlog 10,000→0 |

`SummarizeScriptCost.py` 不只比较两侧文本：校验每帧预期 active/calls/resumes/started/backlog、实际工作量与
每组 72 个进程/36 对业务比较；保存每对总时间、差值、全部分布及每类有效操作成本。所有稳态 CSV 原样保留。
这些单位成本是完整批次的摊销值，不是孤立的 invoke/resume 函数耗时；例如 FlowForge Event 同时包含
新挂起、恢复、Hook 遍历、来源完成及清理，不能把总时间全部归因给单个 resume 函数。
旧 benchmark 不导出 ScriptSystem failures/error 总计，该字段在汇总明确为 null，不补零。成功退出和准确业务
计数已核验；冷期/规模驱动另有真实 errors=0；真实失败路径由 runtime 测试检查。EXE-local new 及 Lua VM
统计都不代表所有 DLL 的全局分配量。

## 6. 最终资格与三方结果

### 6.1 构建与正确性

B2 的独立 clean clone 为 `lux-engine-sr3-cost-final`，构建/安装分别为
`build/RelWithDebInfo/sr3-c3/{t,d}` 和 `install/sr3-c3/{t,d}`。全部执行 `target all -j 4 -- -k 0`，
构建、CTest、消费者、性能串行。Toolchain 107/107（76.04 s）；Developer 113/119（78.88 s），
CTest 退出码分别 0/8。两者第二轮构建均无工作，安装退出码均 0。

六项历史失败：`scene_script_lua_runtime_test`、`scene_script_lua_runtime_workers_0`、
`scene_script_lua_runtime_workers_2`、`scene_script_lua_runtime_workers_4`、
`scene_script_lua_runtime_interpreter_test`、`scene_script_lua_runtime_interpreter_workers_4`。
均是既有 `activeContinuationCount()==0U` 断言（fixture 第 489 行），进程 `0xc0000409`。
本轮 B0/B1 的原 clean clone 全量构建刷新及六项失败重跑有独立日志；不把历史记录直接作为本轮结果。
最终 B2 不称全绿，也不将这六项列为本轮新增失败。

| 保留断言 | 实际验证入口与关键结果 |
|---|---|
| 七点有界重入 | lifecycle `testProtectedReentry`：CREATE/PREPARE/INVOKE/CONTINUATION_DESTROY/RELEASE_METHOD/BACKEND_DESTROY/LEASE_RELEASE 各一条 REENTRY_OK；另一实例可调用、busy 返回、每实例 create/destroy/BeginPlay/EndPlay 恰好一次，总 prepared/release=6、lease acquire/release=2，continuation 分支 destroy=3 |
| 同步退休后的错误 | 同一函数的 `true` 分支额外执行 INVOKE 点，准确断言一条 INVOCATION_FAILURE、status=32、mount=1；未借 sameIncarnation 恢复准入 |
| Bindings 回滚与复用 | `script_bindings_test`：错误 Entity 导致部分发布撤销，重复连接/发布、遍历中 withdraw、128 次重建、最终 calls=130 |
| 新票据与旧身份拒绝 | `testInvocationAuthority`：越界/空身份/旧代次/退休后调用拒绝、移动 pin=1、new generation、prepared 地址稳定；真实 owner 操作，补充而不替代 backend 测试 |
| SR-2 反馈、回收与失败 | lifecycle 的 resolver 拒绝、同批失败/成功、反馈消费和背压、接收后失效、混合批次回滚、incarnation/迟到输入断言原样执行 |
| Event、copy pin、frontier 和预算 | 原 event_wait/continuation/ingress_frontier/runtime fixtures 保留 occurrence cutoff、嵌套 dispatch、copy 中退休/关闭/增长、旧 admission、stale pop 和预算断言 |

成功测试的 stdout 位于 `Testing/Temporary/LastTest.log`，不是只保留 CTest 汇总。Toolchain 原始日志中
七点 + 同步失败共 8 条 REENTRY_OK、1 条 INVOCATION_AUTHORITY_OK、1 条 BINDINGS_OK。

### 6.2 正式性能与安装补充

| 场景 | B0→B1 中位 % [min,max] | B1→B2 | B0→B2 |
|---|---:|---:|---:|
| C++ update | +92.07 [+77.98,+101.50] | -49.18 [-52.86,-45.49] | -7.06 [-11.83,+11.86] |
| Event fan-out | +5.47 [-4.42,+12.49] | -12.05 [-14.49,-5.70] | +3.45 [-11.72,+39.87] |
| FlowForge Event | +19.35 [+16.96,+22.64] | -3.87 [-9.51,-0.89] | +13.17 [+7.76,+24.18] |
| FlowForge update | +31.55 [+30.99,+34.02] | -28.95 [-30.72,-25.52] | -8.14 [-10.43,-3.19] |
| Lua Event | +7.18 [+1.31,+10.24] | -2.57 [-3.46,-1.31] | +3.02 [+1.29,+6.01] |
| Lua update | +7.63 [+1.68,+17.18] | -4.48 [-10.59,-1.61] | +2.34 [-0.57,+6.05] |

每格是该组五对独立进程的配对百分比；三组不同时运行，不用百分比相乘代替直接 B0→B2。
全部五对数值在 `cost-results.json`，异常对不剔除。C++ 有一对 +11.86%，不能把中位改善写成每次必然更快。

| B0→B2 场景 | B0/B2 批量中位 ms | 实际操作数 | B0/B2 ns/操作 | 配对差中位 ms |
|---|---:|---:|---:|---:|
| C++ update | 160.2323 / 146.7523 | 30,000,000 script calls | 5.341 / 4.892 | -11.1453 |
| Event fan-out | 2.1626 / 2.1288 | 10,000 resumes | 216.260 / 212.880 | +0.0679 |
| FlowForge Event | 252.4912 / 292.3955 | 600,000 resumes | 420.819 / 487.326 | +34.2861 |
| FlowForge update | 304.7485 / 284.6209 | 30,000,000 script calls | 10.158 / 9.487 | -24.3722 |
| Lua Event | 1911.6925 / 1986.0631 | 3,000,000 resumes | 637.231 / 662.021 | +58.1080 |
| Lua update | 77.1161 / 78.5412 | 3,000,000 script calls | 25.705 / 26.180 | +1.7853 |

“两个总时间的中位数之差”不等于“配对差的中位数”，fan-out 就出现了不同符号；表格保留两种统计，不挑选
有利者。FlowForge update 每 script 有两次 provider call，因此 B0/B2 为 5.079/4.744 ns/provider call。
按配对差中位计算，FlowForge Event 回退约 **57.14 ns/实际 resume、0.1143 ms/帧**（2,000 resumes）；
Lua Event 约 **19.37 ns/resume、0.1937 ms/帧**（10,000 resumes）。这是本机固定工作量影响，不外推真实游戏帧。

### 6.3 冷期与装配规模

八配置/共享 Hook：首次 prepare 一个实例，随后同批七项迟到，最后只重建一个实例；16 次 warmup、128 次
计时重建。下表包含完整 fixture 的 Entity 销毁/创建、提交、反馈和 lifecycle，不只是一个 owner 方法。

| 冷期对照 | prepare ns | 7 项迟到 ns | 128 次 remount ns |
|---|---:|---:|---:|
| B0→B1 | 120,800→137,900 (+13.39%) | 7,800→11,200 (+51.35%) | 81,000→100,000 (+24.28%) |
| B1→B2 | 136,000→134,400 (-3.24%) | 10,900→11,000 (+7.84%) | 105,000→101,600 (-0.20%) |
| B0→B2 | 136,000→133,400 (-3.75%) | 7,600→11,100 (+44.74%) | 80,000→102,300 (+27.63%) |

括号仍为五对百分比中位；完整分布见 `diagnostics/cold-scale-results.json`。各进程 create/destroy=152、
prepare/release=456。B0→B2 迟到批次的两个中位总数差约 3.5 μs；重建约增加 174 ns/次（两个中位总数差/128），
这些回退没有因稳态 update 改善而删除。

| 单配置 128 次重建对照 | 配置/endpoint 数 | 前/后中位总 ns | 配对中位 % [min,max] |
|---|---:|---:|---:|
| B0→B1 | 8 | 75,300 / 93,600 | +24.49 [+23.18,+26.43] |
| B0→B1 | 8,192 | 372,500 / 96,000 | -74.68 [-76.29,-72.28] |
| B1→B2 | 8 | 92,400 / 91,700 | +0.00 [-2.66,+1.66] |
| B1→B2 | 8,192 | 93,300 / 97,300 | +3.62 [+0.64,+44.83] |
| B0→B2 | 8 | 73,100 / 91,700 | +25.10 [+23.15,+26.85] |
| B0→B2 | 8,192 | 348,600 / 96,400 | -70.96 [-72.89,-64.50] |

B2 两规模均只重建一个实例，诊断均触及 128 个提交槽位、复制 0 个 endpoint 计数项；ID 二分比较仍存在，
没有将其报成常数指令数。B0 的这些窄诊断计数不可用（counter_available=0），不得把占位零当成测量。
业务 ticks=N、create/destroy=N+144、errors=backlog=0；其他实例始终保留，资源最终释放。8,192 的 B1→B2
仍有 +3.62% 中位及一对 +44.83%，未单独归因，也不以规模复杂度改善掩盖它。

独立诊断的 EXE-local prepare/late/remount 分配为 5/33/535，规模 8/8,192 的重建分配为 535/530，双方一致。
运行时 C++/Event/Lua 和额外 FlowForge 诊断的 EXE-local new 为零，Lua VM 数据另列在 CSV；新增的 DLL 内
固定 authority backing 不能靠这些 EXE-local 零计数隐去，实际结构增量已在 §2 列明。

### 6.4 安装与停止判定

14/14 安装消费者通过：cpp-generated-script、physics2d-script、system-event-await-runtime、flowforge-compiler、
lua-script-packager、scene-script-runtime、script-ability-codegen、system-hook-script-binding、cpp-coroutine-script、
script-static-ability-specialization、script-ability-ipo、script-authoring、script-runtime-input、script-description。
6 组 lifecycle + 8 组 Event 严格轨迹对拍相等；每个插桩替换命中数、CASE 和业务事件计数均验证。
wire v1 为 288 bytes，SHA-256 `8002ffd5678f822d79949b4d0a36d1687613216af4b26b876f967fa24cc76e52`。
最终 DLL 的 26 个导出名称与 B1 相同；安装检查确认私有 owner/access 头未安装，direct runtime 无 World/L3
链接闭包，description leaf 无 Process/runtime composition 反向依赖。安装头、DLL、EXE、generator 和生成
投影身份均保存在归档索引。lux-cxx 源码/安装头 114 个交集按 CRLF 归一后全部匹配，未升级依赖。

**结论：已实施窄改并完成本轮验证，SR-3 成本仍未收口。** C++/FlowForge update 的持续大回退已被移除，
但 C++ 配对仍有波动，FlowForge Event 的 B0→B2 五对全部回退，Lua Event、迟到/小规模重建及其他残余
不能据此称为必要安全成本。当前采样与机器码只解释了部分开销；强制内联/分支拆分的撤销证据和保留安全检查
的改进证据已交付，不能证明剩余全部不可避免。不宣称架构收益抵消这些成本，也不自动接受或开始 SR-4。

## 7. 原始记录、无效试验与限制

原始目录名称保持阶段身份：`sr3-closeout` 是首次复核、诊断、中间 C2 三方配对；`sr3-c2` 和
`sr3-c2-checks` 是 C2 的资格/安装；`sr3-cost-final`、`sr3-c3`、`sr3-c3-checks` 对应 B2。
最终目录的 `final-b0-b1` 是本轮先前相同驱动、相同工作量、相同固定 B0/B1 的原始记录副本，未更换产物标签。
正式 B1→B2、B0→B2 在 B2 安装完成后重新运行。
`sr3-closeout` 中已有的文件名 `b2` 指当时的中间 C2（9406），以其 commit/DLL identity 为准；
最终 B2（99c1）的对应文件位于 `sr3-cost-final`。早期 working-tree 诊断 EXE 的内嵌提交 stamp 可能仍旧，
每次真实 DLL SHA 与 patch 已另行记录，不能当成 clean commit 资格或最终 B2 数据。

无效试验不是性能数据：WPR 被系统策略拒绝；一次 Flow 采样缺 linker；一次采样器在进程退出后读取已删除的
临时 DLL，后修成发现模块时记录哈希；诊断代码的编译/格式失败未运行旧 EXE；第一次候选输出路径过长造成
MSVC C1083，因此保留失败树并改用短 `sr3-c2`/`sr3-c3` 输出目录。工作树 tracked-snapshot 检查因受保护修改
正确拒绝，随后改在 clean clone 资格。每条原因及文件名见 `diagnostics/invalid-trials.json`。

采样仅为主线程 RIP 的墙钟抽样，含启动、关闭和暂停开销，无 ETW CPU 周期、调用栈或硬件缓存计数；PDB
inline 名称不能当成实际独立调用次数。只用正式无采样进程的耗时作性能表。波动和异常值保留，不用删除慢对
或只与中间候选比较来宣称收口。旧四个证据包保持原状，本轮仅新增交付包和相对路径索引。

## 8. 提交与证据交付

- `9406f72c`：紧凑权威记录、不可复制票据和 owner 负面/复用断言。
- `99c1d095`：来源/恢复准入查找复用、受保护身份传递，以及同量测量/采样/归一化工具。它是最终 B2 资格 SHA。
- `7129a486d01c741f714f2831693e8d777c351794`：本轮原始证据，两包 2,269 + 1,396 个条目，无编译二进制。

[证据索引与固定提交下载](evidence/script/sr3-cost/README.md) 提供逐文件路径、SHA、源码/安装身份和重放入口。
[远端重取结果](evidence/script/sr3-cost/remote-verification.json) 已从 GitHub 的上述固定提交重新读取两包：
全部 3,665 个文件哈希匹配。归档 SHA 分别为
`e6a489a5e73fe5f5de7c4d2a108ce5f9e83f17ccf9e395c0229920ab9228345b`（复核/归因/C2）和
`cb3d49cd184f5d4137fe519877a69ae92e0d8c56971c44fd3305c22e72e2d098`（最终 B2）。

本轮未知测量脚本、两个未跟踪文件和 main 的五处修改保持原状。后续报告/证据提交仅改变 `.internal/`，
生产与测试代码仍等同 clean qualified B2；没有把文档提交的 SHA 写入旧 EXE 或替代其产物身份。
本次交付停止于 SR-3，等待独立审阅；剩余回退尚未解释完毕，没有自动接受或转入下一阶段。
