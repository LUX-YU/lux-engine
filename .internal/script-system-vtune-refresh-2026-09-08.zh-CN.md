# ScriptSystem：当前版本 VTune 再采样（2026-09-08）

本轮重新采集，不引用旧结果冒充新采样，也不修改生产代码或再建构建树。
分支入口 `3ccef2ba4184eb81f6d4830c2137a5f3ab3f9957`；生产源码与
`402d5ca5ba0c2728bb4bdedda289f9eb36ce7e4e` 完全一致，复用逐项 SHA 核验后的 RelWithDebInfo 产物。
lux-cxx `3100f54d`、toolset `99c3d048` 未变，main 七项未知修改未变。

## 结论

当前瓶颈按调用形状分开：

1. 极短 C++/FlowForge Update 仍主要受通用派发、handler 遍历、调用适配与状态查验的固定成本影响。
2. 持续 FlowForge Event 主要是执行协议记录的访问、登记、完成、恢复与回收；ScriptExecution 所在 core DLL
   自身占全进程 sampled CPU 约 80.5%～80.8%，不是 FlowForge 状态机体或保存/恢复 C++ 调用栈占主导。
3. Lua Update 主要是进入 VM 时的 registry 引用读取、调用适配及 VM 边界；Lua scalar 额外受 prepared/资格查询影响。
4. typed record 主要是反复受保护的 Lua table 操作及 VM 内部工作；Lua coroutine/Event 还具有约 14% 的 GC 链开销。

不能把这些占比解释成相同百分比的可消除成本。未获得硬件 PMU 数据，无法判断 cache miss、内存带宽或分支误预测根因。
本轮不授予性能等价，也不以新采样关闭此前 Lua Update/typed/尾部的性能债务。

## 实际范围与可信度

- Intel VTune 2026.3.0；9 个场景各 2 次，共 **18 个有效新采样**。CPU 软件采样，10 ms、调用栈；不与构建/测试并行。
- 使用匹配 LuaJIT DLL/PDB，以及本次运行期间捕获的 FlowForge AOT DLL 重新符号化；保留原始项目、stdout、CSV 和产物 SHA。
- 全部实际工作量逐行检查，候选 invocation_errors=0，关闭资源计数和业务结果可见；typed/record 独立成功断言保留。
- 这是脚本基准中的 CPU 画像；FlowForge 场景通过真实 Simulation/TaskGraph 执行，其他路径按各自夹具执行。
  不代表一个完整游戏/渲染/物理场景的系统瓶颈，不包括 GPU、Android、Lua54 或其他机器的采样结论。
- 百分比以该进程全部 sampled CPU 为分母，包含启动、warmup、计数和输出；不将它转换成孤立 invoke/resume 的纳秒。
  `physical` 表中的自身时间排除子调用、包含已内联指令；`top-down` 的 inclusive 范围相互重叠，**不可相加**。
- C++ Update 的 CSV 输出约 16.6%～16.7%，Lua Update 约 2.9%～3.1%；它们是基准输出开销，不是游戏运行时瓶颈。
- FlowForge 两次有效 Update sampled CPU 为 8.38/8.52 秒、Event 为 8.67/8.68 秒；本轮没有重现上一组
  同 DLL 的 18.28/8.65 秒悬殊，但不能因此反推已经解释了旧异常。
- LuaJIT 仍有无法归属模块/符号的样本：Update 约 8%～11%、scalar 约 14%～16%、typed 约 4%～6%。
  未解析样本不能都算成某项检查或 GC。生成 AOT 的 `func@RVA` 虽无函数名，已绑定具体 DLL、RVA 和反汇编。

## 1. C++ 与 FlowForge 同步 Update

| 物理函数自身（不含子调用） | C++ Update | FlowForge Update |
|---|---:|---:|
| `ScriptExecution::invoke` | 54.0%～56.0% | 36.8%～37.5% |
| `invokeHookLane`（含内联的 handler 遍历） | 10.0%～10.6% | 7.2%～8.9% |
| CppStatic 入口（含已内联 tick） | 14.4%～16.6% | 不适用 |
| Native `invokePrepared` | 不适用 | 14.3%～17.2% |
| AOT Update wrapper `0x180001080` | 不适用 | 18.7%～20.3% |

真实链路是 Hook → Bindings handler 遍历 → Execution::invoke → CppStatic 或 Native prepared 入口 → 脚本/provider。
`invokeHookLane` 中有一次区域保护；其物理自身开销还包含每 handler 的遍历与 published 判断，
不能把上表 10% 全称为“每 endpoint 固定保护成本”。Invocation 已只借用，析构不再减少一个逐 handler pin。

[Execution::invoke](../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptExecution.hpp#L890)
仍读取调用状态、完整身份、方法范围、同步/异步类型，准备 frame、间接调用 backend，处理返回与故障。
`invokeAccess` 在两个同步样本中约占 C++ 全进程 14.0%～17.2%、FlowForge 约 7.9%；
它的全部机器指令地址包含在 invoke 内，**已经内联，没有额外 call/ret**。

[Native::invokePrepared](../engine/domain/simulation/scripting/native/src/NativeScriptBackend.cpp#L358)
仍检查 frame/prepared 指针，保存 native_instance/user_context，设置 AOT 所需 context，间接调用生成入口，再恢复 frame。
这一层仍是实际函数边界。O2 已将生成 wrapper 中原先额外的生成函数调用合并，不能把剩余成本描述成“还没开编译优化”。

**发现一个窄候选点：** 同步入口返回后，源码 956 行先计算 `access.current()`，957 行才判断 `status==0`。
本次汇编仍先执行 ACTIVE/完整代次比较，随后检查成功返回；成功分支直接结束。
`current()` 只是只读状态查询，可进一步验证是否把它移到非零错误分支，保留外层保护与错误记录。
本轮仅记录，没有修改，也没有宣称已量化该改动的收益。

## 2. 持续 FlowForge Event

每份有效样本的计时区保持：10,000 实例、10,000 个 Hook/occurrence、1 亿 Hook 候选、
2,000 万新调用、2,000 万 resume/provider 完成、8,000 万 single-flight 跳过，末尾 backlog=8,000。
跳过数由固定 Hook 次数和实际新调用数推得，不是硬件分支计数。

不重叠的主要高层链路：Hook 派发约 44.0%～44.5%，Event occurrence 派发约 18.8%～19.8%，
稳定点约 33.6%～35.1%。分别展开后为：

| 子链（inclusive，彼此可能重叠） | 全进程 CPU 占比 | 实际工作 |
|---|---:|---|
| `waitEvent` | 15.9%～16.6% | 来源/身份访问、结果预留、waiter 登记和回滚准备 |
| `eventSource` | 2.6%～3.7% | 固定 mount slot、ACTIVE/完整身份、admission epoch/local index；已内联进 waitEvent |
| `reserveAwaitable` | 6.3%～6.5% | 结果槽位及相关链表状态，不等于每次 malloc |
| `completeClaimedEventWaiter` | 16.8%～18.4% | claim 后检查存活、查结果、copy pin、完成、来源 unlink/清理 |
| `resumeOne` | 31.2%～32.2% | 关联校验、领取结果、backend resume、返回后重查、重新挂起或清理 |
| `takeAwaitable` | 9.5%～11.2% | 结果/通知关联检查与结果移交，是 resumeOne 内的一部分 |
| `destroyContinuation` | 5.2%～5.6% | 解除方法 single-flight、执行链/来源处理、backend/frame 回收 |

物理自身热点还包括 `findExecutionInstance` 和 `findActiveSlot`：两边各约 6%～9%，分散在登记、完成与恢复路径。
[resumeOne](../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptExecution.hpp#L736)
在用户代码前后确实有多次记录定位；不能把所有查找都归成同一个、可直接删除的重复检查。
当前 awaitable/continuation/frame 使用有界存储；“槽位申请/归还”与“通用堆分配”必须区分。

因此，此场景后续值得检查的是**各阶段的记录访问次数、短期查验复用和执行协议的固定成本**。
采样没有支持“主要瓶颈是事件名字查找/保存恢复传统调用栈”，也没有证明删掉 waitEvent 整条功能是必要手段。

## 3. Lua 普通 Update 与 scalar Ability

普通 Update 的 `lua_rawgeti` 物理自身占 20.8%～21.4%，Lua backend invoke 自身约 17.4%～18.4%；
registry/table 访问链 inclusive 约 29.2%～30.2%，VM pcall/call 链约 19.6%～22.5%。

[LuaScriptBackend::invoke](../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp#L1586)
每个 Entity scope 调用会从 registry 取 **traceback、prepared 函数、self table 共三次**，
建立 ExecutionScope、进入 pcall、处理结果并恢复 Lua 栈。这里不是重新按字符串找脚本方法；函数在准备时已经取得引用。

scalar 场景在上述进入 Lua 的基础上，Lua 再调用生成的 `lux.Probe.read`：
生成 projection → [current](../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp#L2025) →
读取 owner/slot/layout upvalues → 核对 active_execution/thread/layout → prepared slot → capture 初始资格 →
参数读取 → provider → 结果转换。

- current 整条链约 **24.8%～25.9%**；包含其中的 capture 约 8.9%～9.0%，不能再相加。
- 全部单个 int 参数读取链约 **8.9%～9.9%**；其中数值类型/有限性/整数范围转换约 5.2%～6.9%。
- 所以 scalar 的主要新增层次是 prepared/上下文/资格访问，而不是“全部耗时都在参数类型检查”。
- 初始资格、自定义转换后重验与 Lua 错误保护承担不同契约；本轮未删除其中任何一个。

## 4. typed record、独立输出与 Lua coroutine

完整 typed 使用真实生成的 `ValuePose{key, velocity{x,y}, mode}` 输入与输出，Lua 检查全部独立结果，
每份 1,000 warmup 后实际完成 1,000 万次 provider，errors/backlog=0，Begin/End 与清理断言保留。

| typed 子链（inclusive） | 占比 |
|---|---:|
| 受保护值操作 `LuaValue.cpp::run` | **59.5%～60.8%** |
| 输入 table shape 检查 | 26.1%～26.9% |
| 逐字段读取 | 13.5%～14.1% |
| 逐字段写入 | 17.1%～19.0% |
| 输出 table 建立 | 5.1%～5.7% |
| current / 转换后 revalidate | 2.6%～3.1% / 1.1%～1.2% |

输入/输出字段范围嵌套于总调用；不能把上表相加。
[生成 record](../modules/function/script/lua/include/lux/engine/function/script/lua/LuaValue.hpp#L386)
固定字段，但每个 shape/field/table/setField 仍进入一次
[受保护 Lua 操作](../modules/function/script/lua/src/LuaValue.cpp#L67)：查 trampoline、准备栈、pcall、恢复栈。
按当前成功路径的生成代码静态展开，Pose 输入有 2 次 shape + 5 次 field，输出有 2 次 table + 5 次 setField，
共 **14 次内部受保护操作**，不含外层脚本调用。这个次数来自源码展开，不是 VTune 的调用次数测量。
shape 先遍历实际键并读取期望键检查缺失，后续字段转换又读取同一批字段；这是可核对的重复 table 访问。
后续可验证合并这些 VM 操作的收益，但必须保留 strict table、构造清理及错误/重入边界，不能机械地把拥有型 C++ 临时对象塞入一次 pcall。

两字段独立输出每份实际执行 4,000 万次：protected run 链占 82.4%～82.9%，
其内的表创建/写入、Lua 栈/键操作、错误边界与分配都在耗时。物理 malloc/free 合计约 22.3%～23.7%。
此独立 consumer 使用 std::realloc 宿主 allocator；不能把这一比例直接套到使用 LuaJIT allocator 的完整 runtime。
这里没有证据表明主要时间在拷贝两个 C++ 字段；单纯改成共享内存并不能自动消除这些 VM 操作。

Lua coroutine 的 GC 链约 14.4%～14.6%，Lua Event 约 13.8%～13.9%；它们还叠加 core 的登记、
恢复与回收。GC 是这些场景当前真实的成本来源之一，不能一律算给 core 的 resumeOne。

## 工作量差异与剩余限制

- C++ Update 每份计时 15 亿脚本调用；FlowForge Update 每份 10 亿脚本调用、20 亿 provider 调用。
  脚本业务不同，不能把两者总 CPU 秒或每个 provider 值直接当语言性能横比。
- Lua coroutine 和 FlowForge Event 实际预算为 2,000，末尾 backlog=8,000。
  **Lua Event 夹具本来就以 size 配置预算，即 10,000**，每轮全部完成、backlog=0；CLI 虽仍带 2,000，
  `runLuaEvent` 内传入的是 `options.size`。本轮没有改变这一既有行为，不能把它写成所有场景实际预算相同。
- 本轮只做新采样与源码/汇编分析，没有新运行时修复、没有重跑全 CTest/安装/配对性能矩阵。
  当前热点与上一轮“新旧候选的回退幅度”是两个问题，不能用占比变化证明加速或债务已消失。
- 下一步优先验证同步成功返回分支、Lua registry/上下文访问次数、Event 记录访问、Lua 值保护操作粒度。
  上述是基于观测的候选方向；未测得收益的假设不作为已定位的硬件根因，也不在本轮自动编码。

## 无效尝试与原始记录

硬件探针失败保留，未声称 PMU 可用。首个 C++ 原始采样有效，初版校验器误将带场景名的 cleanup 行
当成缺失；修正解析后复核同一原始样本，原 manifest/脚本保留。第一份 Flow Update 的应用工作完成，
但采集被链接器 vctip 子进程拖延，已结束采集并从最终占比分析排除；随后仅排除 vctip 的尝试又触发
VTune 注入 link.exe 的 thread_manager_impl 断言，没有成功应用输出。这两次原项目、stdout 和失败原因保留。
最终四份 Flow 采样使用 runtime `trace:notrace`，采集 runtime 及其加载的 AOT DLL、排除编译器子进程，均成功。

VTune top-down CSV 会在带引号的模板名之前加树形缩进；解析器保留缩进后解析 CSV，并校验所有列与数字，
没有静默丢弃带逗号的 C++ 模板函数。8 份新汇编导出核对 inline 地址和同步返回顺序。

[原始采样、调用栈、汇编、输入身份、工作量与失败记录](evidence/script/vtune-refresh-2026-09-08/README.md)。
[上一轮实现与配对成本](script-system-region-optimization-2026-09-08.zh-CN.md)保持原实际资格身份。
