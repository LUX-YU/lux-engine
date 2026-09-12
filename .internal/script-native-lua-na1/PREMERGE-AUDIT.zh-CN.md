# 脚本模块合并前审计（2026-09-12）

后续实施与当前状态见 [清理结果](CLEANUP-RESULT.zh-CN.md)；下文保留修前审计和原始身份，不重标为修后验证。

结论：**当前不建议直接合并。** 找到两个在封存运行库中可复现的正确性缺口、一处源码与机器码确认的自复制问题，以及具体的冗余与成本机会。它们不要求推倒七组件架构。

本轮按“先确认”的要求完成审计与复现，**未修改生产代码、模板或构建规则**，未合并 main。下面的修正措施尚未实施；不能把本报告提交当成新的生产资格。普通调用与协程的最新业务性能仍引用既有测量，不将审计变成一次未授权更换编译参数的性能实验。

## 1. 范围、身份和证据

- 审计分支：`codex/s6-native-task-lua-steps`，入口 `3888b0d11973340a67dc30e8309716b581b3ebc6`。
- 实际生产、封存 E 系列镜像与资格源码：`841320a36e864145eda43fd6b775a10a696d8e7f`。841→3888 的差异只有交付文档/证据。
- 受保护的原始工作区：`lux-engine/main@f89e186216ca904055a6be7ffdccabaae3befff1`，7 项未知修改未改动。其状态和文件哈希单独登记。
- 固定安装依赖 `lux-cxx@3100f54d`、toolset `99c3d048`；Lua 5.5.1、现有 VM patch、INC、16 MiB 空页预算、Native ABI6 不变。不使用 A1/H1 的头或 DLL。
- [本轮身份与封存镜像哈希核对](../evidence/script/premerge-audit-20260912/audit-identity.json)、[独立诊断编译/链接命令与退出码](../evidence/script/premerge-audit-20260912/probes.json)、[文件索引与哈希](../evidence/script/premerge-audit-20260912/files.json)。未提交诊断 EXE/OBJ/PDB；它们的身份保留在记录内。
- 原完整业务、采样、内存、安装资格与历史债务沿用 [LOCAL-WAIT-RESULT](LOCAL-WAIT-RESULT.zh-CN.md) 和 [local-result.json](local-result.json)，不重复改写一套性能账本。

审计覆盖：七个 core owner，Scene 装配入口，CppStatic、Native/FlowForge、Lua、NativeLuaTask 组合路径，普通调用/等待/恢复/取消/退休，Lua codec 与 allocator，FlowForge continuation lowering，公开头与生成/安装消费者衔接。**不是对 Render/Physics 算法、整个 Lua VM 或全引擎每个子系统的形式化证明。**

| 复核区域 | 具体检查 | 结论/发现 |
|---|---|---|
| Instances / Preparer / Bindings / System | 配置冻结、固定地址、访问借用、发布回滚、退休清理、移动与停机、反馈消费、派发顺序 | 现有 owner 分离保留；反馈消费可改进 F09；不重做已经通过的移动赋值补正 |
| Execution / EventWaits | 固定结果 bank、直接实例链接、Event page、claim、pin、槽归还、single-flight、Ready FIFO | 没有重新出现独立 EventWaiterRecord+完整结果记录的旧重复结构；F03–F06 |
| Timers / Ingress | 本地与 external 区分、期限、来源取消、回调重入、迟到完成、frontier/预算 | F02/F08；外部 transport 的原子与 lease 不能按本地 Event 的单线程假设删除 |
| CppStatic / Native / FlowForge | frame 租用、准备描述、start/resume/destroy、生成初始化与存活区间 | F05/F07；FlowForge 已采用实际跨挂起存活分析，不能再声称全 frame 都被无条件清零 |
| Lua / 组合 backend | 同步与协程入口、转换资格、主栈执行域、同步步骤、错误与清理 | F01；协程恢复和同步步骤已有的重验不能证明普通同步入口也正确 |
| 值转换 / allocator | consteval plan、方向、临时对象、pcall、raw field 时序、页预算、统计分离 | 默认 plain record 已有专用计划；allocator 的 Track=false 已编译期去统计，不重复开相同优化 |
| 生成 / SDK / 测量 | 旧入口、VM 分支、ABI6、消费者、业务计数、内存输出与身份 | F10/F11；活动生产路径未发现恢复 Lua54/JIT 支持 |

## 2. 合并前应修的正确性问题

### F01 / P1：普通同步 Lua 导出在参数转换后没有重验原调用资格

位置：[LuaScriptBackend.cpp](../../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp)，`invokePreparedSync()`（1769–1833）；[ScriptExecution.hpp](../../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptExecution.hpp)，`invokeSync()`（1033 起）。

实际链路：

`Event callback → ScriptExecution::invokeSync（入口有效）→ invokePreparedSync → 自定义 record push → 销毁 Entity → 创建 ExecutionScope → lua_pcall → 执行 Lua 函数体`

`pushArgument()` 可以调用自定义转换；此处不能把“进入 backend 前有效”当作“转换之后仍可开始执行 Lua”。普通同步路径没有在转换前保存原资格，也没有在转换后重验。上层同步成功的早返回不是本问题的修复点：等回到上层，Lua 业务已经执行。

独立探针沿用真实 `lua_value_runtime_test.cpp` 的 ValuePose、provider、runtime 和事件 fixture，把目标改为普通同步 Event callback；转换函数实际销毁 Entity，并把一个只计数的 C 函数放进转换结果。Lua 函数体调用它，直接观察业务体是否执行，**不以 failures 非空作为 oracle**。

[原始输出](../evidence/script/premerge-audit-20260912/lua-sync-converter-retire.log)：

| 情况 | 转换次数 | Lua 业务体次数 | 应有次数 | 结果 |
|---|---:|---:|---:|---|
| 实例保持有效 | 1 | 1 | 1 | 合法控制组 |
| 转换只 requestStop，执行区域结束后应用 | 1 | 1 | 1 | 保留已批准的延迟停止语义；后续 stable 未获接受（日志 success=0） |
| 转换实际销毁 Entity | 1 | **1** | **0** | 复现缺口 |

三种情况都没有创建任务 Lua thread，关闭后 prepared 槽归零。探针退出 1，表示业务 oracle 失败；这不是编译/环境失败。它不把“登记停止请求”错误地等同于“当前调用立即失效”。

修正落点：在普通同步入口按原来源捕获资格，仅在可能重入的转换区间之后重验同一资格，拒绝失效调用并恢复准确栈 base。保留 standalone 与绑定 authority 的区别，保留合法 BeginPlay/EndPlay、嵌套和 deferred stop。结果转换失败不能假称回滚已发生业务。无需恢复每次方法范围/签名全查。

### F02 / P2：Timer 时长边界在 Windows 上可能越界转换为 int64

位置：[ScriptTimers.cpp](../../engine/domain/simulation/builtin/script/src/ScriptTimers.cpp)，`planWait()`（77–89）与同形的 `realSeconds()`（160–165）。

当前写法先把 `int64_max` 转成 long double，再用 `requested > maximum` 检查，随后 `ceil` 并转整数。当前 MSVC 的 `sizeof(long double)==8`，这个浮点 maximum 已向上取整为 2^63；边界输入等于它时通过检查，但不能转换成有效 int64。

[实际 runtime 输出](../evidence/script/premerge-audit-20260912/timer-overflow.log)：`9223372036.8547764` 秒在通用 completion 与 owner-local 两条 simulation delay 入口都创建了一个等待，`errors=0`，应返回 `DURATION_OVERFLOW=2`。随后正常关闭，backend/frame 精确释放。探针退出 2，表示错误输入被接受。

该输入约 292 年，现实触发概率低，但接口已经承诺结构化溢出错误，不能依赖越界转换后的机器结果。[C++ 浮点转整数规则](https://eel.is/c++draft/conv.fpint)明确说明无法表示的结果属于未定义行为。

修正应使用可表示的排他上界，在 cast 前验证向上取整结果；同一转换 helper 覆盖 simulation 与 real delay，同时保持原先等待容量与 duration/source 的失败优先级。新增相邻 `nextafter` 边界、非整数纳秒进位和 elapsed 叠加溢出用例。本轮动态探针覆盖两条 simulation 路径；`realSeconds` 是同形源码发现，**没有另称已经运行该分支**。

### F03 / P2：ScriptOwnedBytes::resize 存在同缓冲区 memcpy

位置：[ScriptRuntime.hpp](../../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptRuntime.hpp)，`resize()`（159–174）。

已有非空 inline 数据再次 resize 到 inline 范围时，`inline_.data()==data()`，却仍执行 memcpy。当前优化机器码也保留了该 memcpy 调用。它既没有工作价值，也不应依赖重叠 memcpy 的行为；[MSVC memcpy 合同](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/memcpy-wmemcpy?view=msvc-170)要求源和目标不重叠。

只需在 spill→inline 且复制字节非零时搬运；inline→inline 只更新长度。保留对齐、spill、失败保留旧值、move、自移动行为。**这是源码/机器码确认，没有伪造运行时崩溃复现；正常新等待的 size 原为零，不能把这项收益乘以每一次 Event 等待。**

## 3. 冗余与性能候选清单

优先级 P2 表示应进行窄清理/有界验证，P3 表示冷路径或未量化优化机会。完整操作证据见 [机器码摘录](../evidence/script/premerge-audit-20260912/machine-code-excerpts.asm)、[当前编译命令](../evidence/script/premerge-audit-20260912/compile-commands-selected.json)、[真实布局](../evidence/script/premerge-audit-20260912/layout-excerpts.txt)。这里没有把源码行数等同于耗时。

| ID | 优先级 | 已确认现象 | 可采取的窄改动 | 必须保留 / 证据界限 |
|---|---|---|---|---|
| F04 | P2 | `ScriptExecution.hpp` 的 `resume_enqueued` 只有定义和四处写入，整个生产源码无读取；`awaitableId()` 只有定义。Lua backend 的 `ScalarStorage/readAbilityArgument` 无调用者 | 删除死字段/写入/函数，删除旧 Lua scalar 解码残留 | 不删仍在用的 `ResumeRecord`：它是恢复期间栈快照，不是旧宽 Ready 队列。删除一个 bool 未必缩小 240 B record |
| F05 | P2 | `BoundedClassStorage::release(Ticket)` 校验 page/slot，然后重建 Allocation 并 call `release(Allocation)`；机器码含 0x40 栈分配、临时字段写入和真实 call；后者又做 page/slot 校验 | 两个 checked 外部形状分别解析一次，进入同一个私有 releaseResolved | owner、完整 generation、active/double-free 检查不删；外部 Allocation 的 data/size 校验保留。不是把 H1 分支整体合入 |
| F06 | P2 | 结果取出需要搬离 record；`PreparedResumeType=24 B`、其 optional=32 B、`ScriptOwnedBytes=64 B`、完整结果=88 B。私有 admit 链传值/移动类型描述；`record.value={}` 又经过通用移动赋值/清理 | 同一无用户代码区间只借用 immutable 类型描述一次；直接构造最终 outcome，区分已搬空与仍持有 spill 的释放路径；检查内联小字节搬运与多余元数据清零 | `std::move` 不是零复制；inline 字节仍复制。归还旧等待槽必须早于用户恢复，不能把旧 payload 留在占槽 record 中。具体可省次数还需窄改后机器码/业务计数确认 |
| F07 | P2 | Native `invokePreparedStep` 与 `createNativeContinuation` 都检查 function/step；prepareMethod 已证明 frame 大小/对齐并准备 frame_class，create 时再次检查这些不变项 | checked 入口定位后传局部 step 引用给私有执行内核，复用冷期形状证明 | 当前 slot 容量、lease、真实 packet、generation、实际 step outcome 和 destroy 顺序仍检查；不能删外部 ABI 描述校验 |
| F08 | P2 | Timer `completeDue()` 在无回调区间用同一 ID 两/三次访问 waits SlotMap；完成回调可能已取消来源，返回后还尝试 cancel | 一次定位并复制必要 association/route/external 关系；用明确结果表达来源是否已解除，减少确定重复路径 | 不跨回调持 moving SlotMap 引用。external completion 的 lease 副本有寿命作用；Timer queue-full 的保留重试不同于 Event。重复读取是否已被 CSE 消除尚未量化 |
| F09 | P3 | `ScriptInstances::collectChanges()` 每次部分消费后 `vector.erase(begin, begin+count)` 搬动剩余队列 | 有界 head/cursor，批次结束或必要时整理 | 保留首次变脏顺序、零缓冲区不消费、只确认实际输出、未消费结果背压。不是每帧每 handler 的热成本 |
| F10 | P2 | benchmark 的 `finishRuntimeBenchmark()` 把 `system.failures().size()` 输出为 `idle_page_backing` | 改为准确的 retained failure 名称；真实内存字段只从 allocator stats 来；同步对应解析器 | invocation_errors 是另一个累计量；有界日志条数不等于错误总数，未采集内存仍为 null |
| F11 | P3 | `cmake/installed-consumers/script-lua-value-costs/main.cpp` 的默认非 SR5_VALUES 分支仍引用已删除的 LuaRecordMarshaller，CMake 却已指向 Lua55 | 将历史对照驱动退出当前消费者目录/明确历史用途，或删除失效分支；历史结果与原提交保留 | 它不在当前 16 项正式消费者集合内；本轮未把它编译失败冒称正式 16 项回退，也不补回旧 public API |

F04 定位：Execution 120/185/240/407/547/1119，Lua backend 1624–1670。
F05 定位：BoundedClassStorage.hpp 59–68 与 Allocation release（261 起）。
F06 定位：Execution `reserveAwaitable/admitAwaitable/acquire`、`AwaitableOutcome`、`takeAwaitable`（618 起）、`releaseResolved`。
F07 定位：NativeScriptBackend.cpp 470/582/888。
F08 定位：ScriptTimers.cpp 313 起。F09：ScriptInstances.cpp 270–285。F10：script_runtime_benchmark.cpp 1825–1829。F11：对应 consumer main.cpp 60。

## 4. 现有成本：哪些值得继续，哪些不能归罪于框架

复用了**同一 841 源码与 E-common 镜像**的 P4 32 次 Event 完整任务软件 Hotspots，没有重新使用早期 V2/V3 的 longjmp 占比，也没有新采样冒充新结果。[完整平表](../evidence/script/premerge-audit-20260912/existing-P4-E-hotspots.csv)与[采集命令](../evidence/script/premerge-audit-20260912/existing-profile-runs.json)在本轮索引中。

下表是该平表 CPU self time（全表合计 3.57249 CPU 秒），仅表示采样归因，不是每次 API 的独立计时：

| 归因符号 | CPU 秒 | 平表比例 | 对清理的含义 |
|---|---:|---:|---|
| ScriptExecution::waitEvent | 0.396311 | 11.09% | 登记、容量、结果存储和来源提交仍是工作，重点 F04/F06 与短区间访问复用 |
| luaH_Hgetshortstr | 0.219046 | 6.13% | 包含实际 Lua table/self 字段读取，不能全部移到挂载期 |
| ScriptExecution::resumeOne | 0.212427 | 5.95% | 含结果交接和恢复处理，不应全叫“恢复调用栈成本” |
| ScriptBehavior::captureInvocation | 0.165581 | 4.63% | 可检查是否同一短区间重复捕获；F01 同时证明跨 converter 重验仍有真实语义 |
| ScriptOwnedResumeValue::operator= | 0.119198 | 3.34% | 值搬运/复位值得研究，但不能删所有 ownership |
| ScriptExecution::takeAwaitable | 0.103923 | 2.91% | 在释放逻辑槽前后压缩对象操作，保留用户重入时点 |

软件采样会把内联指令归到 helper/运算符；例如 operator== 被列出不意味着存在一层调用。无 PMU 数据，**不宣称查明 cache miss、分支误预测或确切每个检查的纳秒成本**。表中占比不是可直接相加兑现的优化上限。

既有债务继续保留：Lua scalar 116.012→116.846 ns，中位约 +0.70%；NA1 长任务生成 frame 224→352 B；240 B 固定 AwaitableRecord 的 backing 与更小页面结构需要一起计算。清理字段后即使 sizeof 不变，也可能减少存储指令；反之减了 sizeof 不代表 RSS 同比例下降。未重新计时，因此本报告不给收益百分比承诺。

## 5. C++ 实现层面可以怎样帮助编译器

1. **先收敛语义，再内联。** F05 是明确实例：同一 release 边界解析两次，单纯加 inline 没有移除两套职责。当前三份关键 TU 均为 `/O2 /Ob1`；[MSVC 文档](https://learn.microsoft.com/en-us/cpp/build/reference/ob-inline-function-expansion?view=msvc-170)说明 Ob1 限制候选范围，inline/forceinline 都不保证展开。先做私有 resolved kernel 与短叶函数，检查实际 call/栈，再决定是否值得改变局部布局。
2. **避免机械添加 std::move。** 24/32 B 的平凡描述移动仍等于复制；不让可选结果类型沿三层 helper 反复产生临时值。无用户代码区间可用 const 引用/已定位内部访问；小 ID 和 16 B 窄访问仍适合传值，不把一切改成指针。
3. **保留真正需要的结果脱离。** 从 inline buffer 搬到恢复栈通常是必要的小复制；可删除的是多级中间对象、冗余赋默认值、已解析描述重新包装。构造/析构与 release_pending pin 关系先画清楚，不能 memset 非平凡拥有型对象。
4. **编译期信息已有落点。** `ScriptSyncStepCall.hpp` 的静态 shape、类型 ID，Lua immutable CodecPlan 和 plain callbacks 已用 consteval/constexpr/模板展开。不要再次发明运行期签名 hash 缓存，也不要把可变 lux/self 表当作常量。新的模式分支只有确为冷期固定时才改 if constexpr/静态适配器。
5. **热/冷字段按实际布局整理。** AwaitableRecord 240 B 同时含状态、88 B value、32 B Event link、实例链及来源信息。先删除死字段、观察高频访问跨多少 cache line，再评估内部字段重排；不能随手 pack 成未对齐访问或削弱公开 32 位代次。
6. **有界 frame 活跃区间。** 原生任务把 prepared 同步步骤等值跨等待保留，P4 frame 增长是真实成本。可检查仅当轮使用的临时值是否被不必要地提升进 frame；不能缩小 N×R 保证、把跨用户代码借用假定永久有效，或用跨 DLL 的 LTO 声称已经内联。
7. **冷路径与观测分开。** F09/F11 是维护性与冷成本；Lua allocator 已有 `if constexpr(Track)`。只读源码看到统计相关局部变量不代表正式产物仍执行它们。BoundedClassStorage 的运行期开关可作为低优先级候选，但需要比较真实对象/指令，不再为每个 bool 独开矩阵。

本轮没有更改 Ob/LTO/ISA/fast-math，也没有试图把 Lua longjmp 放过 C++ 拥有型对象。

## 6. 不应作为“冗余”删除的部分

| 当前机制 | 保留原因 / 可以变窄的边界 |
|---|---|
| 单一恢复队列 | 跨 Event/Timer/external 合流、真实 step、frontier 与预算仍需要协调；当前记录只有 8 B ID，不能按已经淘汰的宽记录批评 |
| generation 与当前调用资格 | 旧通知、取消后复用和自定义转换重入仍可发生。F01 已提供实际反例；在无用户代码区间定位一次即可，不需每层重新解析 |
| 写入 pin | 只用于可能回调/取消/退休的写入窗口，维持逻辑 active 与物理不可复用容量的区别；纯 Event copy 已有无 pin 路径 |
| external ingress 原子/lease | 外部 provider 可以不在 ScriptSystem 线程中完成，关闭后仍可能回调；本地 Event/Timer 已不经过这套完整外部运输，不把它们混为一谈 |
| Lua 实际参数类型/range/shape | 每次输入可不同，冷期只有函数签名。默认 scalar 已免掉不必要转换后重验；custom 转换不能同样处理 |
| prepared 路径与七 owner | 执行/结果由 Execution；实例权威由 Instances；EventWaits 管理有序 occurrence 下的紧凑候选；Timers 管来源；Bindings 管普通回调。没有发现需要新建巨型 Runtime 或第二 scheduler 的理由 |
| 同步错误归属 | 同步 status!=0 在 sameIncarnation 下仍应被记录；不把 sameIncarnation 解释为重新获得执行权限 |

## 7. 本轮实际验证与限制

- 当前独立资格 clone 的 **Developer 127/127、Toolchain 110/110** 串行 CTest 再运行通过：[Developer](../evidence/script/premerge-audit-20260912/developer-ctest.log)、[Toolchain](../evidence/script/premerge-audit-20260912/toolchain-ctest.log)。源码仍为 841，未重建全量生产库；CTest 的 compile-negative 自己执行编译。
- 首次 CTest 未初始化 VS 环境，compile-negative 因缺 vector/stdint.h 等头失败；原日志保留在 `ctest-without-vs-env/`。初始化原 MSVC 环境后两套全部通过。不把环境失败算成引擎回退，也不丢弃该次记录。
- 两个新诊断使用原 RelWithDebInfo 测试编译选项（测试显式 `/UNDEBUG`）、当前 import libraries 和封存 E DLL。它们按实际业务不符返回非零，正常组与关闭资源检查通过。**没有修改生产源码去制造结果。**
- 诊断搭建时出现源路径重复头、缺 Lua include、Event CONST_REF 签名和从旧 fixture 留下的 stop oracle 不匹配，均属于诊断准备失败；现存原失败记录在 attempt 目录，未保留下来的中间控制台错误在此明确说明，不冒充有效生产试验。最终有效程序与精确命令已保存。
- 本轮未重跑安装消费者、迁址、增量、VM API-check 或性能矩阵；这些当前源码的既有资格见 LOCAL-WAIT-RESULT，**本报告不重新授予它们 PASS**。普通同步高 arity/嵌套当前栈额度建议随 F01 补 API-check 负例，本轮尚未独立复现栈错误，不列为已证实缺陷。
- 未实施 F01–F11 修复，无修后日志，无收益测量。审计完成不等于合并门槛已通过。

建议清理顺序：F01/F02/F03 加真实回归并修正；F04/F10/F11 做死代码和口径清理；F05/F06/F07/F08 在保留同量工作下检查生成代码与成本；F09 仅随反馈接口维护处理。前一组应在合并前闭合，后续性能候选按实际结果保留，不以其他路径收益抵消新增回退。

采用/合并状态仍为 `NOT_APPROVED_PENDING_REVIEW`。未操作 main、未强推、未发布 tag，保留既有历史证据和性能债务。
