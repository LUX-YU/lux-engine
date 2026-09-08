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
| 执行区域、延迟命令、停止与消费者 | 图外与 Scene 实际 Lua 路径通过开发检查；安装资格待最终源码 |
| 派发/调用/恢复热路径 | 区域保护替代每 handler 的 acquire/release；任意容量 ResumeRing 已验证 |
| Event 等待 | 已合并来源/结果预留登记，并建立广播 endpoint 直接索引；保留所有取消与 claim 协议 |
| FlowForge IR 与 frame | O2 与跨挂起最小 spill 已通过开发检查及五组配对；最终资格待运行 |
| Lua 入口、userdata、生成安装 | 资格缓存与默认拥有型输出实验均撤销；保留原 record 规则，最终安装资格待运行 |
| 联合正确性、安装与配对成本 | 待实施 |

仅 RelWithDebInfo；all -j 4 -- -k 0，构建/测试串行。资格从 clean tracked commit 独立 clone。
最终报告分别记录实际实现、通过检查、成本、撤销实验与未完成项；本文件不是新资格通过声明。

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
成本待配对核验，不以源码删除行数宣称收益。

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
关闭前 backlog 为 0；没有将关闭取消充当业务完成。最终 clean 候选资格仍需独立执行。
