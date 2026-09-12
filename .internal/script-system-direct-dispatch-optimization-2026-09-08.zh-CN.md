# 脚本直接派发与后端成本压缩

本轮落实已批准的直接绑定、响应式 Hook 遍历、执行访问复用和后端窄优化。
工作分支为 `codex/s6-deep-optimization`，入口 `0ceedbcd` 的生产内容与
`402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e` 相同，后者是本轮 B0。
固定 lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`、toolset
`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`；不升级、不合并 main。
本轮已交付生产改动和正确性/生成安装资格，**成本门槛未全部闭合**：首次创建/准备及
首次 Hook 挂起登记存在确认后的新增回退。下面逐项登记，不以持续派发收益抵消，
不认定这些残余为必要安全成本。本轮停止于独立审阅，不宣称性能验收通过。

正式资格源码为 `bc2dbfe2270a4b6777207deceb1ad33b78a02920`，生产实现与 `b5de3b6a` 相同。
其后的 `ee394cd7` 只修正混合语言诊断的 prepared 容量；`611f2750`、`aa855b20`
两个未通过成本门槛的实验分别由 `1b390e60`、`40c14536` 撤销。文档/归档提交不作为新测试身份。

## 权限、地址与调用链

七组件的状态所有权不变。Instances 额外拥有按完整 method capacity 一次分配的
`PreparedInvocation` 数组；每个 32 字节记录借用固定 `InvocationState`，保存完整实例代次，
并以 union 存放同步 `BoundScriptCall` 或可挂起 prepared method 借用。
Bindings 的 32 字节 handler 保存其只读地址和冷期选择的 sync/step 适配器。
Preparer 仍准备 backend 资源，Instances 仍负责正式移交与回收；Bindings 不写实例权限。

发布时 `ScriptBindings::publish → ScriptExecution::prepareInvocation →
ScriptInstances::prepareInvocation` 验证 mount 下标、完整身份、方法范围、INITIALIZED/ACTIVE
状态和 backend 入口形状。失败沿原部分发布回滚路径撤销，本批不留下 handler。
同步/可挂起的选择固定在这一时点，不按 backend 重新排序。

正常同步链为：`HookPoint → endpoint → ScriptSystem endpoint Protection →
Bindings 当前可运行项 → 已绑定 sync adapter → 只读 ACTIVE/代次 → backend prepared invoke`。
成功返回直接结束；不再查身份目录、验证方法范围或构造挂起协议上下文。
非零状态仍按原实例记录故障；`sameIncarnation` 只用于错误归属，不重新授予调用资格。

执行区域和 endpoint/ResumeBatch 的物理回收保护继续覆盖用户代码、返回处理、清理和嵌套调用。
脚本 ECS 结构操作仍延迟到安全点；即时脚本故障/原生撤权仍可阻止本批后续调用。
因此没有跨用户代码缓存 ACTIVE，也没有删掉完整代次。外部输入和通知继续走完整验证。
`invokeAccess` 的有检查私有入口保留供 owner 边界与负例使用，发布后的普通派发不调用它。

## Hook 可运行索引

Bindings 拥有按 handler dense 位置编码的 64 位分层位图及“method → Hook bindings”反向链。
它们都在 capacity plan 内预留，运行时位更新不分配。位图高层表示下一层非空 word，
大量挂起时跳过整段范围。全可运行时按原 dense 游标直接访问。

Execution 仍独占方法级 `active_hooks`。第一次成功关联挂起结果后，关闭同方法的所有 Hook 位；
READY 只表示结果已完成，不清 single-flight。旧 continuation 完成或销毁时，仍在原
`clearActiveHook` 时点打开匹配完整代次且仍 published 的位，随后才进入 backend 析构。
这样析构中的合法重入可以开始新调用；旧记录清理不能覆盖新一轮 single-flight。
同实例其他方法与普通 Event 多飞不受影响。

每次用户代码返回，游标重新读取位图；不缓存跨用户代码的 word，不回头访问经过的位置。
withdraw 立即清位，unlink 仍等待原遍历保护；物理删除仍按原 SlotMap swap-and-pop 次序，
并把原最后一项的位同步移动。配置 ID、登记序号和语言不成为新的遍历顺序。

## 执行记录与来源交接

Execution 内部 `ExecutionAccess` 只在已完成入口验证后借用固定实例执行记录，
在无用户代码的 unlink、take、beginSuspension 与 owner completion 段复用定位结果。
恢复入口解析一次实例，直接按已验证 mount 关联读取 Instances 权限；backend 返回后
重新查可能移动/消失的 continuation，重读实例撤权和原调用资格，不重复查实例目录。
短命 awaitable/continuation 的 ID、代次、取消与等待关联检查继续保留。

Timer 使用私有 `ScriptTimerAdmission`：Execution 校验 completion 的 owner、实例、awaitable
及 PENDING/无来源状态后，返回只暴露身份对的不透明临时借用。Timers 在自己的预留存储中登记，
成功后由 Execution 使用原定位结果写来源；不第二次解析同一身份。此段不执行用户代码，
也不改变 Execution 的结果存储。Timer 只持久保存身份对，不能保留临时结果借用。
登记失败没有执行来源写入，按原错误返回；deadline、逻辑容量、背压和恢复时点不变。

ResultWritePin、claim 保护、取消双向 unlink、重复完成、旧通知和队列容量语义不变。
最终结果仍只属于 Execution。稳定顺序仍为 lifecycle → NextStep → simulation delay →
external admission → 按原预算 drain；每次 pop 后接纳同 frontier 的完成，每 occurrence
仍是 claim → callbacks → complete。没有组件独立 tick 或额外恢复点。

## C++ 与 Native Ability

C++ 直接 System、`ScriptAbilityStatic<Ability, Provider>` 与运行时可替换的
`ScriptAbilityCpp` 分别保留、分别测量。静态实例的优化结果不能代替动态绑定承诺。
CppStatic coroutine 的普通 Ability 准备关联现在存放在冷期固定 provider block 中，
按契约本地槽位直接取 context/dispatch，省掉重复的 association/capability 映射。
block 在对象析构之后归还，下一次物化复用；新增 backing 计入现有统计。

Native 新增 `makeScriptAbilityNativeContribution<Ability, Provider>(provider, binding)`。
工厂验证 typed provider、context、dispatch 与契约；contribution 保存预期 publication，
backend 在创建阶段核验一致性，失配明确失败，不自动转回动态实现。
生成的 `Entries<Provider>` 把具体 provider dispatch 固定到模板实例；通用 `Entries<void>`
继续服务实际动态 publication。stateless 合同不引用不存在的 ProviderDispatch。
Native 模块、prepared entry、step 和 completion 的 C ABI 均不变。

迁移包括 installed authoring 的真实三语言安装调用链、Physics 的具体 CaptureProvider、
installed Ability 对照与生成模板。PhysicsQuery 的独立 DLL publication 继续使用其动态契约，
不拿任意 context 强制转换成某个具体 provider 来绕过冷期核验。

## Lua 转换与 coroutine 实验决定

初始 provider 资格判断仍对 scalar/零参数执行。自定义转换之后，重验原执行 frame、
原 thread/实例/slot、稳定 behavior 关联及原权限捕获；不再次完整运行 `current()` 的解析。
standalone 与已绑定但失效的 core authority 分开，BeginPlay/EndPlay 保留专属资格。
准备方法时预热已知参数/结果数量所需的栈容量；没有跨用户代码保存 Lua 栈索引。

strict raw table 形状遍历记录已出现字段，按声明顺序报告缺失字段，不再为存在性逐字段 rawget。
正式字段读取和自定义转换依然发生在原字段时点。类型、方向、enum 集合和 const 构造策略不扩展。

批量输出只适用于内建 scalar、有限 enum 与默认 record 组成的平凡、bounded 值树。
仍使用同一个 Codec 规则体系；有自定义表示、可观察构造或清理的树继续逐字段转换。
typed owning 对象及逆序清理留在外层转换 frame；单个保护操作内部只有平凡节点、
Lua C API 和内建数值读取。节点保存字段借用，直到原字段时点才取值。

首个实验曾提前复制 scalar；新负例证明 Lua 分配回调对源字段的合法修改会被漏掉。
该实验被修正，失败和旧计时保留且明确不作为合格生产收益。修正后的错误路径、深度、OOM、
栈平衡、资源存活及恢复继续受测试约束。结果输出失败不回滚已发生的 provider 业务。
本轮没有把拥有型 C++ 临时对象放进一次 pcall，也没有批量抓取自定义转换可能修改的后续字段。

coroutine 探针保留 `coroutine.running()`、全局和闭包引用，再释放 backend registry 引用并 GC。
旧 thread 仍可达，身份和 dead/suspended 状态可观察；Lua54 还检查 `__close` 清理。
因此否决直接回收这些 thread 的池化方案。本轮不引入 VM 私有可达性接口、语义变化或永久双轨；
同一次执行的多次 resume 继续复用原 thread，不重新创建 VM。

## 验证映射与保留限制

Bindings 的 4097 项测试覆盖层级边界、稀疏/全阻塞遍历、删除换位、嵌套撤销与重发布顺序。
continuation fixture 的 `testSharedHookMethod` 覆盖同方法跨 Hook、READY 仍阻塞及 destroy 重入；
`testHookRatios` 覆盖 0/50/99/100% 挂起、逐步 ingress/预算恢复与最终释放。
`testSingleFlightIsolation` 保留不同方法/实例/代次隔离，Timer 的取消、deadline、背压负例不变。
Lua boundary/runtime/VM fixtures 保留输入拒绝 provider=0、错误 recovery、生命周期资格、
构造逆序清理、四个失效调用负例、OOM 与 thread 身份观察。

`LUX_SCRIPT_HOTPATH_OBSERVATION` 默认 OFF，只在独立诊断构建记录 Hook 候选和实际访问。
统计的 enabled=false 表示未观察，不能把此时的零解释为零访问。计时、计数、VM 分配和 VTune
分开运行；源码推导的 B0 访问量明确标为推导。非零 backlog 与不可观测历史错误不能改写成零。

仅验收列明的 Windows x64 RelWithDebInfo 环境。历史成本债务沿用
[上一轮报告](script-system-region-optimization-2026-09-08.zh-CN.md)和
[VTune 入口](script-system-vtune-refresh-2026-09-08.zh-CN.md)，不重标旧数据为本轮结果。
不授予性能等价，不将全部残余解释为必要安全成本，不增加语言、类型、多脚本、热重载或调度机会。

## 最终正确性与生成安装资格

`delivery.json` 和 `logs/delivery-{t,d,l}` 绑定上述 clean tracked commit 及独立 clean clone，
全量 `target all -j 4 -- -k 0` 后第二轮均无工作。构建、测试、计时和采样串行。

| 配置 | 实际结果 |
|---|---|
| Toolchain / LuaJIT | 全量 CTest 109/109 |
| Developer / LuaJIT | 全量 CTest 123/123 |
| Developer / Lua54 | 受影响集合 110/110 |
| 安装消费者 | 原 15 个全部通过，包含 Native 特化、真实生成 Ability 和三语言 authoring |
| 生成增量 | 原 13 类全部通过；公共头同步 Debug/RelWithDebInfo/Android include |
| 迁址 SDK | 旧 source/build/install 临时不可用时，重新生成/编译/执行纵向消费者通过 |
| wire golden | 288 字节；SHA-256 `8002ffd5678f822d79949b4d0a36d1687613216af4b26b876f967fa24cc76e52` |

迁址执行的独立计数为 legal=2、rejected=2、BeginPlay=1、EndPlay=1、provider 构造/析构各 1、
lease/release 各 1、backlog=0。安装链不读取引擎测试私有头或旧 generated。
私有 owner/访问头未安装；direct runtime 的实际链接不携带 World/Scene/Process，描述 leaf
不携带 runtime composition。后文证据包含实际安装文件、生成器与 DLL 哈希。

原六类 Scene Lua 协议断言在本轮列明的配置中均通过；此前失败及其修正记录仍在旧报告，
不重新称作本轮修复。Windows x64 以外、Android 构建、其他编译器/CPU/真实游戏项目均未验证。

`delivery-protocol` 实测 17 个已完成结果以预算 3 在真实 step 1～6 恢复，末步为 2 个，
shutdown 前已排空；没有新增恢复点。`delivery-traces` 的旧/新非空 CASE、业务计数和生命周期/
Event 轨迹完全匹配。Bindings 64、65、4097 项覆盖 word 和摘要层边界。

旧断言到本轮入口的对应关系：

| 原语义/断言 | 本轮实际执行入口与保留断言 |
|---|---|
| 同步、挂起、多方法隔离 | continuation fixture 的 `testSyncAndContinuation`、`testSingleFlightIsolation` 现在通过发布的 sync/step adapter；`testMixedHookEntryShapes` 保留同 endpoint 原顺序 |
| 同方法跨 Hook、READY、destroy 重入 | `testSharedHookMethod` 走反向 Hook 索引；READY 未清位、destroy 前恢复资格、另一方法可调用；`testHookRatios` 核对真实访问/新调用和预算排空 |
| occurrence 截止与嵌套 | event fixture 的 `testRegistrationCutoff`、`testNestedDispatch`、`testResumeBudget` 仍走 claim→callbacks→complete 和同一稳定点 |
| 写入/生命周期保护 | `testCopyRetirementPin`、`testCopyOtherRecordRemoval`、`testCopyShutdownAndFailure`、`testCopyNestedAdmission` 保留 copy 中取消、其他记录删除、busy/失败、嵌套增长断言；不让定位复用跨越可失效记录的用户代码窗口 |
| 代次、权限与回收 | `testPreparedAdmissionProvenance`、`testIncarnationAndPendingContinuation`、`testOutputSensitiveRetirement`；owner `testInvocationAuthority` 仍核对坏范围/旧代次拒绝和稳定地址 |
| 生命周期、七处清理重入、移动 | lifecycle fixture 原 prepare/asset/BeginPlay 失败计数、`testProtectedReentry` 两模式、`testMoveReplacement`/空/closed/self/busy、执行区域故障/延迟停止断言未削弱 |
| Lua 初始权限、恢复与构造 | 真实生成 value runtime 的 retire-scalar/retire-zero/fault-scalar/fault-zero 保持目标 provider=0；stop 仍按已批准执行区域契约延迟，input-recovery、standalone、BeginPlay/EndPlay 合法调用保持；value fixture 检查 OOM、构造逆序清理和字段时点 |
| Native publication 一致性 | native backend 真实 context/dispatch 各一项失配，output/completion 均未产生，`NATIVE_SPECIALIZATION_REJECTED ... provider=0`；安装消费者运行具体 provider 并核对独立结果字段 |

对应源码仍位于原 `builtin/script/test`、`simulation/scripting/{lua,native}/test` 和
`modules/function/script/lua/test`，没有迁出或用测试名称替代实际业务断言。

## 同量成本与未收口项

主对照为 B0 `402d5ca5` → 资格源码 `bc2dbfe2`。每项五组独立进程，交替顺序，固定 workload、
seed、warmup、容量、VM、worker 与预算。下表百分比为**每对完整批次总时间之比的中位数**，
不是两列 ns 中位数的比值。ns 是实际业务操作上的批次摊销，尤其不是孤立 resume 的耗时。

| 路径 | B0 → 候选，ns/实际操作 | 配对中位变化 | 更快对数 |
|---|---:|---:|---:|
| C++ Update | 4.406 → 3.315 | -24.51% | 5/5 |
| FlowForge Update | 4.521 → 3.590 | -19.57% | 5/5 |
| FlowForge 普通 Ability | 7.257 → 5.613 | -23.06% | 5/5 |
| FlowForge coroutine | 775.247 → 603.165 | -22.42% | 5/5 |
| 持续 FlowForge Event | 402.313 → 330.982 | -16.10% | 5/5 |
| FlowForge sequence | 1325.471 → 1035.550 | -21.13% | 5/5 |
| Lua scalar Ability | 83.910 → 79.488 | -5.37% | 5/5 |
| Lua Update | 26.534 → 25.680 | -2.61% | 5/5 |
| Lua coroutine | 695.362 → 619.782 | -10.58% | 5/5 |
| Lua Event | 567.759 → 568.120 | -0.22% | 混合 |
| 首次 Hook 调用并登记 Event waiter | 173.964 → 184.554 | **+6.70%** | 0/5 |
| broadcast 完成/复制 | 81.193 → 80.645 | -1.77% | 3/5 |
| 对应稳定恢复 | 126.680 → 97.332 | -23.03% | 5/5 |
| 对应取消/退休 | 235.698 → 229.782 | -2.56% | 5/5 |
| targeted 普通 Event 多飞登记 | 204.468 → 201.141 | -2.32% | 4/5 |
| targeted 对应恢复 | 133.045 → 95.977 | -27.77% | 5/5 |
| C++ coroutine backend start | 65.283 → 60.397 | -4.75% | 4/5 |
| C++ coroutine backend resume/destroy | 39.843 → 38.058 | +0.96% | 2/5 |
| Physics 三语言混合 | 518.470 → 452.542 | -13.36% | 5/5 |

普通 Event fan-out delivery 为 +0.23%（混合），对应 resume 为 -32.80%（5/5）；
targeted delivery 为 -1.03%（3/5），cancel 为 -11.87%（5/5）。完整批量总时间、
实际操作数、每对分布、p50/p95/p99 批次时间和末尾/峰值 backlog 在原始 analysis 中，
不把微小波动称作等价。最终错误在计时外采集；旧字段不可观测时仍为 null。
C++ 独立 backend 和 Physics micro 的全量 failures 日志不可观测，只能断言实际 step 返回值、
业务校验和、frame 清空与退出码；不能将空 stderr 当作全域错误数为零。

**仍未满足本轮保留门槛的新增成本：**

- 首次 Hook/等待登记确认轮为 **+5.75%、5/5 更慢**，首轮绝对差约 10.6 ns/次。
  `registerEventWaiters()` 的 broadcast 分支先派发 Hook，再在首次 SUSPENDED 时关闭可运行位；
  targeted 分支通过普通 Event 启动多飞，不走这项 Hook 禁用。两者不是同一种登记成本。
  这证明计时范围包含新维护操作，但尚不足以把全部差额归因于该操作。
- 首次 create+prepare 在最终轨迹为 **+15.39%**，独立确认曾为 +11.28%（5/5）。
  场景是完整 capacity=8、初始只解析 1 个配置；不是一次激活 8 个实例。
  分段诊断 B0 create/prepare 中位 110.1/31.2 μs，候选 119.5/39.9 μs，约多 18.1 μs。
  新固定调用/位图/Cpp provider backing 是待分解候选，EXE-local 分配计数不能证明 DLL 内分配原因。

这两项未因其他收益而获豁免。没有通过门槛的窄实验已撤销；本轮交付状态为
**正确性与安装通过、成本未收口**，需要独立审阅决定后续对这两个保留项的处置。

## 响应式遍历、特化与实验门槛

1000 个 Hook handlers，16 次 warmup、32768 次计时派发；计时内无 resume，
计时后通过原 ingress/预算清理，实际 calls/waits 及最终释放匹配。

| 挂起比例 | B0 → 候选，ns/endpoint 批次 | 配对中位变化 |
|---|---:|---:|
| 0% | 13605.50 → 13386.13 | -1.27% |
| 50% | 11198.71 → 7679.75 | -31.31% |
| 99% | 8681.20 → 168.62 | -98.05% |
| 100% | 8547.24 → 27.527 | -99.68% |

独立开启观察计数的 64 批诊断，在每组 64000 个潜在候选下实际访问为
64000/32000/640/0，新调用相同。计时版统计开关关闭时不能把字段零当作实测访问。
100% 挂起的 ns/new-call 为 null（没有新调用），不是零成本。B0 逐项访问数由源码推导。

Native 特化/通用入口的独立安装对照为 -47.66%、5/5 更快；typed-static 相对直接 System
约多 0.08 ns/操作（约 0.295 → 0.373 ns，批次循环吞吐，不能解释为孤立调用延迟）。
动态 Ability 独立列出。FlowForge 旧 benchmark 本来就使用手写 direct provider，
其收益不归给新增生成特化。生成/真实消费者的冷期失配负例独立通过。

Lua 最终 typed/两字段 record 相对 B0 分别 -23.01%/-31.40%。安全修正后的批量输出，
相对未批量版本的两轮独立门槛为约 -34.70%/-34.22%，各 5/5；两字段输出 protected
操作数从 3 降为 1。每个进程只报告一个 aggregate，未采集逐次转换尾延迟。
相应 10000 次分配诊断为 20081/1122592 bytes → 20000/1120000 bytes，计数范围见原日志。

以下不是合格生产改动：提前捕获字段值（真实 allocation callback 负例失败，已修正）；
endpoint 全同步专门循环（不满足成本门槛，已撤销）；small bitmap inline（两轮未确认 3% 收益，
已撤销）；固定 Event 支持字段/owner 布局变更（登记 -0.05% 的确认中位、混合，已撤销）；
Hook dense 位置缓存（两轮登记约 +0.73%/+2.04%，已撤销）。保留所有成功、失败和变慢的原始记录。

coroutine 复用因 thread 身份负例未通过语义门槛而否决，无生产池化代码。
混合语言 grouped 诊断比 interleaved 中位 -1.79%、4/5，未获 3% 收益门槛；生产顺序不变，
无硬件计数证据时不解释成 cache miss/分支误预测。

8/8192 配置单实例 16 warmup+128 重建最终分别 +2.07%/+1.23%；分布保留慢样本，
不声称等价。两规模均实测提交触及 128 个槽位、endpoint 计数项为 0；实例和绑定地址稳定。
曾发现 stats() 逐 endpoint 汇总位图 backing 的 O(N) 工作，`b5de3b6a` 改为准备期计算后 O(1) 读取。
此修正和优化前后规模记录均保留，不把旧 8192 的回退结果重标。

选定 backing：8 配置 mount 3456→4224 bytes，binding 928→1120；8192 配置
mount 3538944→4325376，binding 950272→1146880；method backing 不变。
这些是现有统计覆盖的存储，不包括全部 vector 头、allocator、DLL/VM 堆或进程峰值。
VM/EXE-local/Native 的独立诊断各自列出，不把不可观测或关闭计数的零拼成总内存结论。

## VTune 与实际机器码

另行串行采集 11 种 workload × 2 次软件 Hotspots（默认 10 ms、带栈），共 22 个原始项目。
实际工具为 `D:/Softwares/Intel/oneAPI/vtune/2026.3/bin64/vtune.exe`。硬件探针失败并提示
采样驱动/管理员条件，因此没有 cache miss、分支误预测或 PMU 吞吐归因。

| 样本 | physical function 自身时间，含其内联代码，排除被调用函数 |
|---|---|
| C++ Update | 短 `invokeSyncEntry` 43.9～48.0%；endpoint `invokeHookLane` 18.1～18.8% |
| FlowForge Update | 生成模块入口 26.8～28.7%；短 sync entry 20.3～21.5%；Native prepared wrapper 15.2～17.0% |
| 持续 FlowForge Event | `waitEvent` 7.0～11.4%；`resumeOne` 6.8～7.4%；Native step wrapper 5.6～6.1% |
| Lua Update | `lua_rawgeti` 16.2～24.8%；backend invoke 约 16.9%；VM 执行和未解析地址分别保留 |
| Lua scalar Ability | `LuaAbilityProjectionAccess::current` 10.8～11.5%；另有 Lua 栈访问、VM 执行和 registry 读取 |
| Lua coroutine/Event | 等待/恢复、投影入口、GC 遍历、分配/释放均有热样本，不将全帧成本记为 resume |
| 两字段 record 输出 | malloc/free 合计约 27.9～31.6%；另有字段读取 thunk、Lua table/value 操作与 protected 边界 |

这些比例的分母是**整个被采样进程的 CPU 时间**，包括 startup、warmup 和输出。
physical 与 inline 两种归属是同一批样本的不同呈现，不能相加；inline 名称不代表发生了函数调用。
FlowForge Update 两次 CPU 样本总时间约 13.71/6.67 s，业务相同但波动原因未解释，原样保留；
不把 profiler wall time/CPU time 用作无采样的配对性能结果。不为任何 invoke 占比设验收目标。

Event micro 的计时 register 子树中，waitEvent inclusive 占其样本约 37.5～45.8%，
beginSuspension 约 27.0～30.4%，其中包含的 setMethodRunnable 约 2.7～4.3%。
后两项重叠，不能相加。位转换样本数量很小，这只能确认调用链中确有新增维护工作，
不能用 10 ms 采样分辨约 10 ns/次差额的全部来源。整个进程还反复创建/回收两个 runtime，
其 malloc/free/容量准备样本不代表登记计时区间内发生了同样的分配。

PDB 地址与反汇编核对：B0 共用 invoke 的栈预留为 `sub rsp, 0xe0`；候选独立 sync entry
为 `sub rsp, 0x30`。正常路径只有内联 ACTIVE 与完整 ID 两部分比较，随后设置 backend context、
累计业务次数、一次间接调用 backend。零返回直接跳到 epilogue，没有调用身份目录、方法范围
检查或成功后 current()。错误分支仍重读身份并记录，未恢复调用权限。这里的寄存器保存与
栈预留不是 coroutine frame 大小。反汇编工具只按最近 export 打印的标题不可信，真实函数边界
以 `machine-code/manifest.json` 的 PDB section/address 和 DLL/PDB SHA 为准。

Native 通用 generated read 的机器码实际包含 dispatch 载入和一次间接调用；特化模板在冷期
固定具体 provider，安装实测保留其独立结果。IPO EXE 的 main/通用入口及符号/哈希一起归档，
不将 inline 在工具中的归属变化误报为性能提升。

撤销实验后 Toolchain 从同一 bc2 源码重新链接，EXE/DLL/PDB 哈希与先前计时产物不同；
记录于 `logs/restored-t`（all、no-op、109/109）及采样 identity。Developer/Lua54 原资格产物保留。
初次 VTune 搜索把同名 Developer DLL 放在 Toolchain 前，造成六份 FlowForge 报表地址误标；
原报表与搜索配置留在 `ambiguous-search-reports`，已按实际加载的 Toolchain 文件重新解析
原始样本，并逐模块验证哈希与 sync entry 的实际 PDB 地址。错误符号报表不参与上述结论。

## 交接入口

[原始证据与重放索引](evidence/script/direct-dispatch/README.md)包含：源码/产物身份、
逐项 SHA、旧断言到实际入口、完整配对 CSV/JSON、原始 CTest/安装/增量/迁址日志、
全部实验与撤销理由、22 份 VTune 原始项目及机器码。支持边界和旧债务继续引用前述旧报告。

原始 main 未由本任务修改：此前记录的六个文件保持哈希；审计期间观察到另一处
ScriptExecution.hpp 修改和一个旧未跟踪文本消失，均未恢复、删除或纳入提交。
固定依赖及其工作区状态未改变。没有合并 main、发布 tag、扩大类型/语言/功能或冻结框架。
