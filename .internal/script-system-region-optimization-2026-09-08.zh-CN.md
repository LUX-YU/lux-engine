# ScriptSystem 受控区域与激进优化实施记录

用户已批准本轮实际实现。入口 `5a309191f08f43e220a9309961f9de677de9d071`，
生产参照 `7b5e1dd4`（与 `f64cadde` 生产代码一致）；lux-cxx `3100f54d`、toolset `99c3d048` 不变。
原 main 的五项 tracked、两项 untracked 修改不纳入提交。原 SR-6 与 VTune 证据不重标、不重打包。

## 已批准契约

- 原生 System 按自己的调度契约直接读写组件；脚本的 ECS 修改通过封装延迟提交。
- 一个 script-capable Hook 区域是一批；执行中实例、绑定和 prepared 存储有效，区域末尾提交。
- 停止/销毁请求不立即失效；未捕获错误只停故障实例，其他实例继续。已接受命令不自动回滚。
- 唯一 stable Hook、真实 step、恢复预算、每次 pop 后同 frontier 接入与 Event claim/callback/complete 不变。
- 保留 Event 等待，优化来源、登记、路由和结果存储；七组件所有权与 backend C ABI 不变。
- 图外调用使用显式受控区域；公开边界与 Lua 保留必要错误检测，内部受信调用不重复防护。
- Lua 默认 record 可迁移拥有型 userdata，允许独立值编辑；子视图保活根值，不借用 ECS 暂存内存。
- 类型范围不扩展，自定义表示保持单一规则和方向；不增加非平凡跨挂起协议。

## 实施与验证状态

| 工作 | 状态 |
|---|---|
| 基线、环境与原始证据固定 | 独立基线 Toolchain 108/108、Developer 123/123；身份见外部 identity.json |
| 执行区域、延迟命令、停止与消费者 | `402d5ca5` 独立 clone 资格及 15 个安装消费者通过 |
| 派发/调用/恢复热路径 | 区域保护替代每 handler 的 acquire/release；任意容量 ResumeRing 已验证 |
| Event 等待 | 已合并来源/结果预留登记，并建立广播 endpoint 直接索引；保留所有取消与 claim 协议 |
| FlowForge IR 与 frame | O2、真实 JIT 与跨挂起 spill 资格通过；匹配存储诊断通过 |
| Lua 入口、userdata、生成安装 | 两项实验撤销；原规则通过 LuaJIT/Lua54、15 个消费者、13 项增量及独立搬迁验证 |
| 联合正确性、安装与配对成本 | 正确性/安装完成；FlowForge/Event 登记改善，Lua 新增回退未全面收口，见最终结果 |

仅 RelWithDebInfo；all -j 4 -- -k 0，构建/测试串行。资格从 clean tracked commit 独立 clone。
以下分别记录实现、实际资格、成本、撤销实验与限制；不将文档提交重标为测试或测量源码身份。

### 首轮开发检查

`script-region-opt/logs/region-dev/build-11.log` 全量构建通过，`ctest-8.log` 受影响集合 75/75。
该目录位于 `E:/SyncForder/CodeRepos/build/RelWithDebInfo/`，使用当前开发工作树，不作 clean clone 资格。
保留此前失败日志：旧停止语义断言、遗漏显式区域入口、测试资产 prepared 容量不足和编译错误。
新增 Lua 夹具分别运行普通 stop 同批合法、真实嵌套 fault 后 provider 为零、输入错误 recovery，
以及 patch/destroy 接受后脚本报错仍提交。不可将未捕获实例错误与基础设施失败混用。
ResumeRing 容量 3、预算 2 的 17 次循环最终实际调用/恢复/析构均 35、backlog 0。

### Scene 组件命令边界

`ScriptRuntimeHost::components` 是原生装配方提供的冻结 typed 描述，`command_capacity` 固定命令数与
payload 字节上限。仅当完整配置包含启用 Entity scope 时才准备队列，未解析配置也计入。
`ScriptRuntimeSystem` 私有拥有 `EcsCommandBuffer` 与 `DeferredScriptHost`；不建立额外 TaskGraph 或 tick。
初始 BeginPlay、Hook/恢复、增量生命周期及关闭清理各借用 writer；所有借用结束后才允许 apply。
现有 Hook `committed` 边界依次是：Simulation 的原生命令 apply → 脚本命令 apply → 生命周期协调。
生命周期回调新登记的命令等待下个 Hook barrier；最终 shutdown 回调的命令在关闭完成后的串行边界提交。
冷安装失败仍是装配回滚，不宣称未成立 Scene 中的命令已经生效。

脚本 enqueue 容量不足返回 false，之前接受的命令不被 poison；提交时旧 Entity 或组件形状冲突拒绝该条，
并继续处理同批其他命令。原生命令默认严格错误策略不变。`commandStats()` 提供接受/拒绝/提交拒绝、
discard 与最后一个提交错误的值快照，不输出可写存储。

`scene-build-2.log` all 与 `scene-ctest-2.log` 9/9 通过。新增 `SCENE_DEFERRED` 在真实 NextStep 恢复中
提交 patch/destroy，同时允许另一实例 provider 继续；普通/故障两支各检查 2 次恢复、2 次 provider、
2 次 EndPlay、backlog 0，并在 0/1/2/4 worker 与 interpreter 既有配置中执行。
原生 System 直接写组件仍合法；脚本读取在当前区域看到旧值，barrier 后才应用自己的修改。

### Event 登记窄改

`eventSource` 复用执行索引的固定 mount slot，并在 Instances 中一次检查 ACTIVE、完整 instance 与 admission
scope/epoch/local index。结果预留到 waiter 提交之间无用户代码；Admission 仅是该短区间的不可复制预检值，
不获得长期调用权限。取消、copy pin、claim 截止序号和最终结果所有权不变。
广播路由固定为 endpoint 数组；定向路由仍有界哈希，直接使用插入返回的迭代器。
`event-ctest-2.log` 17/17；`event-ctest-3.log` 新增 37 次复用通过，高水位 2、74 次恢复、backlog 0。
这一开发检查本身不证明成本；最终五组配对结果见下文。

### FlowForge 通用 IR 优化

AOT 在 lowering、异步状态机、导入与 wrapper 生成及 verify 后运行 LLVM O2，再 verify、生成机器码。
runMainJIT 与 FlowScriptInstance 的 ExecutionEngine 都使用相同 O2 transformer；CodeGenOptLevel 保持原值，
不启用 LTO、fast-math 或新的 ISA。transformer 的借用覆盖创建与符号 lookup。
补测暴露私有 JIT host 漏传 prepared 隐藏参数，现以空 prepared 表保持原 graph-only 支持范围。
真实循环 sink 为 0/1/2/3/4/105；独立 Event 两次参数为 73/91，错误参数与未知 Event 不调用 sink。

`flow-o2-dev` 全量与原 8 项检查通过；`flow-jit-dev-2/ctest-4.log` 新 JIT 通过。
失败夹具与编译日志保留：旧夹具在加入 Graph 前连接而生成空函数，不能作为 JIT 正确性证据。
五组成本见 `performance/flow-o2`：相对 Event 阶段，Update 配对中位 -8.96%（4/5）、
直接 Ability -13.05%（5/5）；coroutine +1.42%、Event +0.23%、sequence +0.92%，后者没有稳定收益。
这些是开发候选测量；源码 patch 随日志保存，不能将带未提交 patch 的 CSV HEAD 当作完整源码身份。
历史 CSV 无计时前累计计数，报告保留全批总时间，摊销只用有相邻计数的第 1 行至末行；不推造首行工作量。

### 调用区域与可信宿主的具体边界

原生 System 保留 Registry 的直接访问。其线程、观察者和结构修改约束仍由 System 作者遵守；
本轮不以脚本封装限制原生 System。向脚本暴露的 patch/destroy 和停止是请求，
不会在当前 Hook 批次内拆掉宿主或 prepared 方法；读取仍观察本批次开始时已经提交的状态。
原生 Ability provider 如果自行持有 Registry，也属于可信宿主代码，必须按调度契约提供封装。

`ExecutionRegion` 是一次公开、不可复制的 owner 借用，明确图外调用的边界；它不是 coroutine frame。
Hook/Event endpoint 内层只领取一次遍历保护，`Invocation` 借用固定宿主与只读 prepared 方法，
不再为每个 handler 增减保护计数。恢复批次同样有一份外层保护。
错误可以在用户代码内重入，因此完整代次与当前故障权限检查、copy/cleanup pin 和外部 completion lease 保留。
正常 requestStop 的接受不会伪装成同步 shutdown 成功；区域中 shutdown 仍返回 busy。
这改变了旧“停止后同批普通调用立刻失效”的业务语义，相关断言改为下一边界生效，不能当作纯机器码等价。

### 已撤销 Lua 拥有型值实验的构造与寿命

以下描述的是已测试、随后因完整链路回退而撤销的实验，不是最终生产支持矩阵。
实验默认生成 record 输出是持有独立 C++ 值的 userdata，representation 版本为 2。
`lux.values[canonical_name](table)` 用原 strict raw table 规则构造；普通输入仍允许相同的构造表，
或读取类型/representation/布局一致的拥有型值快照。自定义 rule 的方向和表示不回退到默认 reader。
字段按 typed member 生成访问代码；不是运行时反射偏移解释器，不增加任意对象图或跨挂起结果协议。
sol2 的对象绑定职责不变。

输出必须可 noexcept 复制和销毁；字段构造使用原 typed 临时槽，不默认构造全部 T。
const 成员可以构造和读取，其 setter 被拒绝；成员编辑只用于 noexcept 可赋值的字段。
嵌套视图引用并保活最外层根值；修改不会反写原 C++ 参数、ECS 或 Event 的瞬时 payload。
注册 descriptor 和关联代码的寿命必须覆盖 VM，不支持热卸载其 DLL。

Lua userdata 分配和元表设置在受保护 Lua 操作中完成，C++ 值随后在 pcall 外构造。
字段转换和复制期间临时保活根值；显式 GC 先撤销后续访问，再等 pin 结束准确析构一次。
getter/setter/constructor 的 C++ 帧先返回结构化结果、清完临时对象，Lua wrapper 再从无 C++ 清理责任的
C trampoline 抛 Lua 错误。没有把拥有型 C++ 临时对象机械塞进 pcall。

`lua-owned-build-10.log` all、`lua-owned-ctest-10.log` 3/3 通过；此前 build-9/ctest-9 的 22/22 覆盖六个
Scene Lua、Event、continuation 和区域检查。错误日志保留：LuaJIT 清理帧错误传播、构造器未注册、
断言及编译错误。它们是开发过程记录，不是最终 clean clone 资格。

### 有限实验与新增回退登记

`performance/lua-owned` 第一候选包括调用帧资格缓存。Update 的 5 个进程对全部变慢，
约 25.43 → 28.20 ns/次；scalar Ability 约 85.24 → 91.01 ns/次。
源码 patch、产物和完整工作量随原始记录保留。调用帧扩大会初始化更多数据只是待验证解释；
`performance/lua-cache-off` 只撤销该缓存：Update 五组均改善，配对中位 -6.406%；
scalar Ability 中位 -4.814%。缓存未保留，原初始资格与转换后重验完整恢复。

`performance/lua-owned-values-2` 的五组完整 typed Ability 约 1393 → 2002 ns/次，
配对中位 +42.96%，5/5 变慢；10,000 次诊断 VM 分配由 80,000 增至 90,000。
两字段单独输出约 249.19 → 136.40 ns/次，中位 -45.49%，但不能以它抵消完整链路回退。
默认拥有型输出也已撤销：生产继续使用原 record 表示，不建立 raw/owned 永久双轨。
原始源码 patch、真实构造/重入/GC 测试、失败日志和观测产物保留在实验目录。
`lua-owned-values` 首次驱动未输出 compile_commands.json，未开始应用测量；修正驱动后新目录重跑，
两次记录分开保存，没有覆盖无效试验。

### 构建空间清理

按用户的插入指令先暂停构建与测量，清理 149 个已被替代的 CMake 构建树。
每个目标核对规范化绝对路径、独立源码位置、无活动进程、无嵌套 Git/重解析入口，
保存并逐文件校验其中的文本/配置/生成代码后，才用 PowerShell LiteralPath 删除。
约释放 103 GiB；E 盘可用空间约 34 → 136 GiB。139.86 MiB 清理记录位于
`script-region-opt/cleanup-20260908/`，含删除清单、源码关联、逐文件 SHA-256 和完成后的复核。
原 main、当前工作源码、固定依赖、当前基线/O2 参照/开发构建、原 SR-6 与 VTune 输入不动。
后续开发复用现有构建树，不为普通迭代再建一套完整工程。

### FlowForge 跨挂起 frame

保留显式 await 结果槽、PHI、参数及原始 alloca；其他 SSA 值不再全部预先 spill。
增加 resume 控制边后，原有有界 dominance repair 只迁移实际跨挂起使用的值，ABI、零初始化及销毁入口不变。
`flow-frame-build-1.log` all，`flow-frame-ctest-1.log` 8/8；新增准确循环/结果/函数业务计数均通过。
`flow-frame-ctest-2.log` 3/3 再验证计时区外的 Native frame 占用、回收与总错误观测。

`performance/flow-frame` 与 clean O2 参照 `67fab903` 五组配对，业务计数与 backlog 逐行一致。
gameplay coroutine frame 40 → 32 字节，整批摊销约 780.81 → 769.44 ns/provider 完成，
配对中位 -1.45%（4/5），包括调用、等待、接纳和恢复；不能称作一次纯 resume 的耗时。
Event 中位 -0.53%（4/5），sequence -0.33%（3/5），不宣称后者有稳定收益。
无 await 的 Update 提前返回同一 lowering 路径，因此本组同步路径的表观差值不能归因于 frame 优化。
后续 Native 存储诊断区分 frame 声明字节、已分配 backing、槽位占用和真实 live bytes。

### 装配与恢复诊断迁移

复用原 SR-3 scale 和 SR-6 protocol 驱动。复制夹具时同步对应源码身份的区域辅助头，
仅把图外调用接入显式公开 region，不替换 provider 工作、实际步号或预算。
插桩替换仍要求准确命中次数。新增 namespace 导致旧宽泛插桩命中 2 次时驱动真实失败；
改为唯一 fixture namespace 前缀后重跑，没有取消校验或把空输出算成功。

`scale-development-2` 两种规模各 16 warmup、128 次单实例重建及独立分配诊断通过。
每组仅访问 128 个配置槽位，endpoint 计数目录访问为 0；宿主地址复用与最终销毁计数一致。
`protocol-development` 17 个 READY 值以原预算 3、真实 step 1～6 恢复，值均为 73、析构 17、
关闭前 backlog 为 0；没有将关闭取消充当业务完成。最终 clean 候选已在下表对应目录独立重跑。

## 最终资格与接口交接

实际资格源码为 `402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e`，独立 clean clone 为
`script-region-opt/final-source`；构建复用 `o/w/{t,d,l}`，安装为 `install/o/q/{t,d,l}`。
工作分支的后续诊断驱动/报告提交不替换这一身份。固定依赖、LLVM/VM 选择与入口身份均随证据保存。

| 检查 | 实际结果 | 原始位置（相对 `script-region-opt/`） |
|---|---|---|
| Toolchain all / 第二轮 / 全 CTest | 109/109；第二轮无工作 | `logs/final-t/` |
| Developer all / 第二轮 / 全 CTest | 123/123；第二轮无工作 | `logs/final-d/` |
| Lua54 all / 第二轮 / 受影响 CTest | 110/110；第二轮无工作 | `logs/final-l/` |
| 原 12 个消费者 + runtime-input / description / lua-values | 15/15；独立生成、运行及第二轮无工作 | `final-consumers/` |
| Lua 值生成增量及负例 | 13/13；未改旧断言 | `final-incremental/` |
| 搬迁 SDK 纵向调用 | 旧 source/build/SDK 临时离线；新路径重新生成、编译、运行通过 | `final-relocation/` |
| 真实步号与预算 | 17 个 READY，预算 3，第 1～6 步完成，关闭前 backlog 0 | `final-protocol/` |
| 装配规模 | 8/8192 各 5 组；16 warmup、128 次单实例重建，slot visits=128、endpoint visits=0 | `final-scale/` |
| 生命周期、Event 与 wire | 6 组生命周期、8 组 Event 轨迹的事件/次数相同；288 字节 golden 相同 | `final-traces-2/` |
| Native 存储 | O2 参照与最终源码使用相同观察器；业务和释放状态匹配 | `final-storage/` |

六项 Scene Lua 路径在本轮实际通过，不再仅以历史失败标签排除。移动、自移动、busy 子进程、
七点清理重入、Bindings 回滚、旧代次、copy pin、frontier 和预算的既有检查均保留在上述集合中。
SDK 纵向链仍断言 legal=2、rejected=2、Begin/End 各 1、provider 构造/析构各 1、lease/release 各 1，
backlog=0；普通输入错误后的合法恢复也实际执行。

首轮 `55f30c3b` 的 Toolchain smoke 失败不是运行时缺陷：新增 C++ checksum 观测放在了 EndPlay 汇总之前。
修正只将检查移至 shutdown 之后，`logs/final-t-55f30c3b-failed/` 保留修前记录。
SDK 首次搬迁的生成和执行已通过，路径审计因混合斜杠误报；修正后在旧输入再次离线时重新生成/编译/运行，
首次日志与原因留在 `final-relocation/first-audit-failure/`。首次完整 trace 驱动遗漏了自身增加的 3 个
跨批 Hook 调用的显式 region，失败记录在 `final-traces/`；补齐后仍严格检查每个插桩的命中次数，
没有放宽轨迹或业务断言。诊断主函数改为无缓冲输出，使下一次失败不会丢掉已经输出的 CASE。

### 七组件所有权及调用链

| Owner | 本轮变化 | 保留职责 |
|---|---|---|
| Instances | Invocation 成为批次内借用；Event admission 复用固定 slot | 完整身份、ACTIVE/fault 权限、prepared/宿主、清理保护与权威状态 |
| Bindings | 无容器所有权变化 | 普通 handler、冻结方法引用、连接 token、遍历保护 |
| Preparer | 无职责扩张 | 冷期验证、稳定地址移交、失败回滚 |
| Execution | 外层 ResumeBatch 保护；合并无用户代码区间内的 awaitable 预留/登记；ResumeRing 有界 wrap | continuation、最终结果、结果 pin、通知、完成/取消/恢复 |
| EventWaits | 广播 endpoint 直接索引；定向路由复用插入位置；短期 Admission | waiter、登记序号、claim、unlink、取消与统计 |
| Timers | 无协议改动 | NextStep/模拟时间来源，G03 显式取消 |
| CompletionIngress | 无协议改动 | 外部 lease、队列、关闭、背压与 publication frontier |

`ScriptSystem` 继续协调这些 owner。Scene 只在现有 Hook 前后与 committed 回调借用脚本命令 writer 和
ExecutionRegion；System 的正式调用链仍是 endpoint → Bindings 遍历 → Execution::invoke →
Instances::invokeAccess → 已准备的 backend 方法。现在每个 endpoint 遍历只增减一次保护计数，
Invocation 自身不再增减该计数。`invokeAccess` 的固定槽位、完整身份、方法范围检查仍存在：
本轮没有把原生宿主的错误、捕获的嵌套脚本故障或跨代次输入变成重新获得权限的机会。
同步方法返回非零错误时仍记录它，`sameIncarnation` 仅用于确定错误所属资源，不能发起新调用。

持续 Event 的路径保持：prepared Event admission → Execution 预留最终结果 → EventWaits 登记 →
返回 awaitable 并关联 continuation；occurrence claim → 普通 callbacks → copy/完成仍有效的 waiter；
唯一 stable 点再按预算 pop/resume，每次 pop 后仍以同 frontier 补接入。没有把普通 Event handler
变成等待，也没有把 Hook single-flight 扩大成实例互斥。FlowForge 的 frame 是显式无栈状态机存储，
不保存/恢复 C++ 调用栈；LLVM O2 与跨挂起 dominance repair 只改变生成代码及需要存活的值。

### 旧断言到新入口

| 原断言/场景 | 新入口与实际语义 |
|---|---|
| 图外普通 Hook/Event/稳定恢复 | 对应 fixture/consumer 使用公开 `beginExecutionRegion`；嵌套调用借用外层区域，业务断言保留 |
| BeginPlay/EndPlay、部分准备、绑定回滚 | 仍使用原生命周期 construction/retirement 资格，不强行要求 ACTIVE |
| 转换内 requestStop 后 scalar/零参调用 | 本批保持合法；shutdown 返回 busy，后续安全边界关闭；这是经批准的语义变更 |
| 转换内实际原生退休后 scalar/零参调用 | 原两个负例仍 provider=0，完整身份/权限不被跳过 |
| 转换内嵌套 Lua fault 后 scalar/零参调用 | 新两个独立负例 provider=0；错误总数和故障状态均检查 |
| 输入错误 recovery、standalone、同 VM 嵌套 | 原合法调用/独立字段结果保留，不使用全局错误 latch |
| 脚本 patch/destroy 后继续访问 | 本区域读取旧值，命令接受后在已有 barrier 提交；未捕获错误也不撤销已接受命令 |
| Event cutoff/nested/copy/pin/cancel | 原轨迹和次数保留；新增广播 37 次复用，高水位 2、74 次恢复 |
| 非 2 次幂 ResumeRing | 容量 3、预算 2、35 次调用/恢复/析构，最终 backlog 0 |
| 真实 Scene 调度与 Lua 组件 | 原六种配置增加 `SCENE_DEFERRED`，覆盖普通/故障两支，另一实例继续执行 |

Scene 的默认脚本封装只暴露已装配的 typed 组件。Lua 的 `self:destroy()` 返回“请求是否接受”，
不会在当前函数内立即销毁实体；`patch_component` 同样如此。原生 System 的 Registry 访问没有被禁止，
其作者仍负责线程/观察者/结构修改契约。直接 runtime 的宿主须显式装配 `DeferredScriptHost` 或等价的
受控延迟封装；公开 C++ 宿主契约不可能阻止一个自行绕过它的原生 provider。
`ExecutionRegion::finish()` 结束借用，不自带额外 tick/恢复或命令 apply；图外 owner 必须在自己的已有
安全边界结束 writer、apply 命令，再协调生命周期。移动一个仍有活动区域的 runtime 属于违反借用期，
不会静默丢弃资源。跨线程完成仍只能使用既有 ingress 能力，不能跨线程调用 owner API。

Lua 类型/方向/构造支持矩阵继续使用
[SR-5 实施记录](script-system-sr5-implementation-2026-09-07.zh-CN.md) 和
[SR-6 资格](script-system-sr6-qualification-2026-09-07.zh-CN.md)。strict raw table、有限 enum、
opt-in typed record、自定义方向、非默认构造和 const 成员规则不变；未加入 ECS 内存借用或任意对象图。
原始性能债务与未验证环境也沿用这些记录，本轮结果不授予历史性能等价。

## 最终成本与归因

### 同量比较的身份和计数

主比较是入口 `5a309191` → 最终 `402d5ca5`，两边均重新构建，生产入口与 `7b5e1dd4/f64cadde`
相同。不能把 SR-3/SR-5 的更旧债务重新标为本轮新增；既有债务沿用上面的 SR-6 记录。
阶段间 `entry-event`、`flow-o2`、`flow-frame` 用于区分贡献，不能只拿较慢的中间候选证明最终收益。

`performance/final` 是 13 个场景、5 对、130 个独立进程；交替先后，不与构建/VTune 并行。
固定 10,000 实例、seed=1592598566，CLI 预算 2,000；Lua Event 既有夹具实际按 size 使用预算 10,000，
其余相关积压场景为 2,000（两侧实际行为相同）。Scene 1,000 warmup/5,000 frames，
scalar 5/1,500，Event 窄阶段 5/30。VM、实际 EXE/DLL/PDB、参数和退出码随 runs/identity 保存。
没有锁频或指定核心亲和性，因此配对分布与尾部均保留；不能根据指令所在地址猜测 cache miss。

C++/Lua Update 各完成 5,000 万次实际新调用；FlowForge Update 是 5,000 万次脚本调用和
1 亿次 provider 调用。持续 FlowForge Event 的计时区有 5,000 万 Hook 候选、1,000 万新调用、
1,000 万 resume/provider 完成、5,000 个 occurrence；其余 4,000 万为 single-flight 跳过。
这些计数来自固定 Hook 次数及运行时业务计数差值，属于工作量核对，不是硬件分支计数。
持续 Event/coroutine 的末尾 8,000 continuation/awaitable/queue 是两边相同的预算积压；
没有删除它们换速度，关闭回收与关闭前自然排空的协议检查分别记录。

每一行已有业务字段逐项比较。候选总 invocation_errors 在计时外观测为 0，并检查清理；
旧 CSV 没有该字段时保留 null，不能把进程返回成功写成旧全局错误数为零。
`analysis.json` 同时保存全部行总时间、有效操作数、每操作摊销、p50/p95/p99、backlog 与内存字段。
下表时间为各侧进程中位数；变化为每个进程对比例的中位数，两种统计不能混为一个比值。
Flow 的旧 micro 数据缺首行前累计计数时，全部批次总时间仍保留，单位成本仅用第 1～末行的可观测差值。

| 路径 | 入口 → 最终 ns/实际操作 | 配对中位变化 | 更快进程对 |
|---|---:|---:|---:|
| C++ Update / 新调用 | 4.724 → 4.625 | -5.281% | 3/5 |
| FlowForge Update / provider | 4.517 → 3.993 | -9.360% | 5/5 |
| FlowForge Ability / provider | 6.847 → 6.234 | -7.370% | 4/5 |
| FlowForge coroutine / 完成 | 784.585 → 774.881 | -1.261% | 4/5 |
| FlowForge Event / 完成 | 446.552 → 403.480 | -9.389% | 5/5 |
| FlowForge sequence / 完成 | 1349.867 → 1307.353 | -2.997% | 5/5 |
| Lua Update / 新调用 | 25.843 → 27.000 | +2.498% | 2/5 |
| Lua scalar / provider | 83.101 → 85.349 | +1.378% | 1/5 |
| Lua coroutine / 新调用 | 715.742 → 691.931 | -2.812% | 4/5 |
| Lua Event / 新调用 | 609.315 → 577.005 | -4.716% | 5/5 |

这里的 coroutine/Event 单位时间包含整批调用、跳过、登记、完成、恢复和维护，**不是纯 resume 耗时**。

| Event 窄阶段 | 入口 → 最终 ns/参与操作 | 配对中位变化 | 更快进程对 |
|---|---:|---:|---:|
| event-fanout-10k / fanout-delivery | 86.480 → 77.770 | +0.961% | 2/5 |
| event-fanout-10k / fanout-resume | 131.870 → 122.570 | -5.590% | 4/5 |
| event-register-1 / register | 216.244 → 177.026 | -17.533% | 5/5 |
| event-register-1 / deliver-copy | 80.729 → 79.212 | -2.134% | 4/5 |
| event-register-1 / resume | 130.182 → 127.650 | -1.166% | 4/5 |
| event-register-1 / cancel | 252.978 → 239.805 | -4.979% | 5/5 |
| event-targeted-10k / register | 238.502 → 204.649 | -14.049% | 5/5 |
| event-targeted-10k / deliver-copy | 84.384 → 84.592 | +0.088% | 2/5 |
| event-targeted-10k / resume | 134.625 → 133.810 | -1.993% | 3/5 |
| event-targeted-10k / cancel | 112.264 → 109.900 | -2.179% | 4/5 |

广播/定向登记的五对分别稳定改善约 17.5%/14.0%；delivery、resume 和 cancellation 的小变化按分布阅读。
fan-out delivery 五对从 -10.43% 到 +14.66%，配对中位 +0.96%，不能拿各侧中位数的下降宣称稳定改善。

### Lua 的新增回退与撤销诊断

最终原始五对的 Lua Update 约 25.843 → 27.000 ns/次，配对中位 +2.498%；追加的同量确认五对
中位 +5.395%，范围 [+1.833%, +7.141%]，五对均慢。scalar 原五对 +1.378%，确认五对 +0.463%
且范围 [-2.814%, +2.039%]，后者不能判为稳定新增回退；两组均保留。

完整 typed Ability 的 500,000 次操作：原五对 1368.703 → 1417.004 ns/次，中位 +3.879%、5/5 慢；
确认五对 1370.097 → 1394.545，中位 +2.647%、5/5 慢。每进程 provider、字段结果、errors=0、
backlog=0、Begin/End 和资源归零均核对。两字段 raw record 单独输出的五对为 247.274 → 246.315 ns/次，
中位 -0.331%，确认组 -1.299%；不宣称有稳定收益，也不把它当完整 typed 链路。

针对新增成本做了两项有限诊断，均未进入生产：

- `typed-region-probe`：同一正式 SDK，保留每 Hook 一个公开 region，去掉 fixture 辅助函数的两次
  状态查询。五对中位 -1.061%，范围 [-3.187%, -0.165%]。这能证明这两次查询有小成本，不能解释全部
  typed 差距；该无条件入口也不替代支持嵌套/已关闭情况的通用测试 helper，不能改测量入口来消掉回退。
- `lua-destroy-probe`：相同 EXE、三种单独 DLL，45 个进程。去掉新增 destroy 装配的诊断在 Update
  上中位 +3.471%，没有支持“每实例闭包导致回退”的假设；这一侧缺少功能，不能作为生产候选。
  每 VM 共享闭包实验的 Update 中位 -6.816%，但配对范围 [-13.743%, +16.725%]，typed 中位 +1.385%。
  original/shared 的真实 Lua conformance 通过；未证明足以支持收益归因，实验撤销，不追加兼容路径。

所有更慢但正确完成的测量都保留为有效成本样本；它们不是“无效试验”。
**Lua Update、完整 typed 以及下述 Lua coroutine p99 的新增成本尚未全面收口。**
本轮没有把它们认定为必要安全成本，也没有拿架构或其他后端的收益授予抵消。

### 批量、尾部、恢复和内存

以下为五进程各自分位数的中位数，单位为一批的微秒。它们不是单次 provider 的尾延迟。

| 场景 | 全部计时批次总时间 ms（入口 → 最终） | p50 µs | p95 µs | p99 µs |
|---|---:|---:|---:|---:|
| cpp-update-10k | 236.199 → 231.235 | 47.700 → 45.500 | 55.500 → 54.400 | 95.100 → 87.700 |
| flow-update-10k | 451.728 → 399.306 | 79.500 → 72.300 | 108.800 → 105.900 | 144.800 → 139.000 |
| flow-event-10k | 4465.520 → 4034.797 | 882.700 → 792.100 | 1061.800 → 970.000 | 1170.200 → 1087.800 |
| lua-update-10k | 1292.130 → 1349.983 | 234.000 → 239.300 | 333.600 → 348.400 | 407.500 → 434.000 |
| lua-coroutine-10k | 7157.421 → 6919.307 | 1298.800 → 1261.700 | 2404.500 → 1948.200 | 4087.100 → 4882.800 |

Lua coroutine 的总耗时和 p95 下降，但 p99 从 4087.1 → 4882.8 µs；不能称作尾部全面改善。
不额外启动逐项微调。恢复协议的独立诊断在 step 0 有 17 个 READY，预算 3；
step 1～5 各恢复 3 个，step 6 恢复 2 个，关闭前完成/析构均 17、backlog 0。
延迟以真实 step 记录，没有为了更好数据新增恢复点、改 deadline 或提高预算。

Native 同一观察器比较 clean O2 `67fab903` 与最终源码：coroutine frame 40 → 32 字节，
10,000 槽 backing 400,000 → 320,000，稳定期 8,000 活动帧 live 320,000 → 256,000；
metadata 240,312 不变、heap frames 0。Event frame 48 字节和 480,000 backing 不变。
这些是 Native 受控存储量，不是整个进程 RSS；`acquires/releases` 原字段来自 allocator 的算法步数，
不能读成 frame 构造/分配次数。最终 active/live/occupied 归零、业务次数与独立析构测试共同闭环。

计时轮关闭 allocation accounting，零字段表示不可观测；10,000 次独立值诊断保留 VM 分配量，
EXE-local operator new 仅观察该 EXE 链接范围，不能代替 DLL/VM 分配统计。
冷期诊断两边 EXE-local prepare/late/128 rebuild 分别都是 5/33/535。

### 冷期与装配规模

`final-traces-2` 同时完成五组原冷期驱动：完整 create/prepare 的中位 140.4 → 144.5 µs；
七个迟到实例一批 11.1 → 11.8 µs；128 次单实例重建 105.7 → 104.2 µs。
配对中位分别 +0.570%、+8.257%、-1.135%，前两项范围跨零，迟到批次范围 [-12.903%, +11.927%]。
每组 creates/destroys=152、prepared/releases=456，16 次 warmup 后固定计时 128 次重建。

`final-scale` 的 8/8192 配置各只反复重建同一个实例，每个计时阶段只触及 128 个配置、0 个 endpoint
计数目录项；所有配置 backing、方法/绑定范围和地址复用检查保留。每次重建中位分别
743.750 → 748.438 ns、775.000 → 768.750 ns；两种规模配对中位 +1.376%/+2.218%。
8 配置候选第 1 对有 +128.556% 的真实慢样本，未排除，完整 p50/p95/p99 和单次样本在原始目录。
只能据计数证明没有重新引入随总配置数扫描，不能据一个中位数宣称两规模性能等价。

### VTune、实际调用边界和机器码

`final-vtune` 有七场景各两次、共 14 份软件 10 ms 栈采样；typed 比较再有入口/最终各两份，
共 18 个原始项目。使用匹配的 LuaJIT DLL/PDB 和实际运行期间捕获的 AOT DLL 重新符号化，
初次导出仍保存；分析使用 `resolved/`。没有硬件 PMU 资格，不能据此声称 cache miss、IPC 或分支预测根因。

- C++ Update：invoke 的 inclusive sampled CPU 约 64.3～66.1%；invokeAccess 的内联子范围约 13.4～17.9%。
  实际反汇编中 invokeAccess 的 42 个指令地址全部包含在 invoke 内，从 `0x180086d85` 开始。
  没有一个额外的 invokeAccess call/ret；固定槽位、代次/ACTIVE、方法范围和宿主读取仍是实际指令。
  因此继续给它加 inline 不能移除这些操作。每 endpoint 的一次保护和遍历与每 handler 的借用/查验须分开计费。
- FlowForge Event：invoke inclusive 约 39.9～44.9%，resumeOne 30.3～35.3%，waitEvent 14.6～17.9%，
  其中 eventSource 2.9～5.0%、reserveAwaitable 5.0～5.5%。这些范围相互包含，**不可相加**。
  eventSource 的 72 个机器指令地址全部在 waitEvent 内；直接 endpoint 路由减少登记搜索，
  并没有移除结果分配、waiter 链接、完整身份或 claim/copy/取消。优化后的剩余不能一律归给 resume。
- FlowForge Update：旧 wrapper 在 `0x18000107f` 调用另一生成函数；新 wrapper `0x180001080` 已直接
  包含状态更新和两次 prepared provider 间接调用。新的独立图函数末尾也使用尾跳转。
  LLVM O2 改变了真实机器码；没有只给候选开 LTO、fast-math 或新 ISA。
- Lua scalar：current 约 22.2～24.1%，capture 5.9～7.0%。新 region 不消除初始调用权、
  自定义转换后重验、参数类型/数值范围及 Lua 错误边界；已撤销 authority-cache 实验说明缓存并非免费。
  standalone 与绑定后失效仍分开，Lua pcall 捕获普通输入错误后的合法调用不受全局 latch 影响。
- typed：两边采样总 CPU 约 13.04/12.84 对 13.50/13.30 秒；begin/finish 各自最多约 0.56%/0.57%。
  这不足以把全部回退归因于 region。LuaJIT 动态生成且仍无法解析的范围明确保留为 unknown。

两个 Flow Update 采样的 AOT 全 DLL SHA 相同（`55f68c5f21e476c94893d28424375486f53dec3658327ae6a560db5b4d437232`），
实际工作量相同，但 sampled CPU 为 18.28/8.65 秒，差异原因未解释。两份保留，
不将任何 profile 总时间混进无采样的五对性能；符号化后占比变化也不能当作性能改变。
反汇编 CSV、dumpbin 原始输出、调用指令列表和 inline 地址包含性检查随证据保存。

## 交付状态与边界

实现和列明环境的正确性/生成安装资格完成；性能结论分路径，不授予统一 PASS。
保留的生产变化是区域/延迟命令、借用粒度、Event 登记/路由、ResumeRing、有证据的 FlowForge O2/frame；
Lua owning record、authority cache、shared destroy 和 helper 诊断均未进入生产。
新 Lua 回退保持未收口，不伪称等价或全部属于必要安全成本。

本轮只验证 Windows x64、MSVC、LuaJIT/Lua54 的列明 RelWithDebInfo 集合；
Lua54 未跑完整性能矩阵，Android/Linux/Player/Editor 的独立运行资格和硬件 PMU 未授予。
不扩类型、不借用任意 ECS 内存、不新增多脚本/热重载/恢复点/调度器，不合并 main 或发布 tag。
原 main 七项未知修改和固定依赖再次逐项核对未变，见 `final-protection-check.json`。

[原始证据与固定下载入口](evidence/script/region-optimization/README.md)包含源码/安装身份、
全部配对、失败与撤销试验、构造/回收轨迹、18 个 VTune 原始采样项目及导出。
原始采样不是新机器上的复跑保证，重放仍需对应源码/符号/VM、工具链和环境。
后续文档/归档提交不替换 `402d5ca5` 的实际资格身份。
