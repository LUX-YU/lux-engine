# 当前 ScriptSystem 性能瓶颈：VTune 调查，2026-09-08

本次按用户要求实际使用VTune定位已验收脚本实现。**生产代码、模板、依赖和安全契约没有修改。**
当前分支入口为`d2d24f056010141b235ac8d684ed088e05b5f78e`，实际分析产物仍来自
`7b5e1dd4f824e0a2746b1dbb0ec2d0dcc57fe490`；与`f64cadde`生产/模板一致。
这是针对当前脚本代表工作量的独立归因，不是整个引擎/真实游戏瓶颈证明，也不重开SR-6资格或创建SR-7。
原验收、有限支持与成本债务继续引用[SR-6摘要](script-system-sr6-summary-2026-09-07.zh-CN.md)。

## 可以据此优先调查的三个位置

1. **持续Event：等待登记及身份/来源查验链。** FlowForge Event的核心runtime DLL占79.6–80.8%全进程采样CPU，
   `waitEvent`含子调用占31.2–33.2%，其中`eventSource`含子调用占11.1–12.5%，
   `createAwaitableRecord`含子调用占4.8–5.6%。`resumeOne`整条路径另占25.4–25.6%，
   它与登记路径有嵌套，**不能相加**。Lua Event也有20.8–22.2%的`waitEvent`，并有10.5–11.1%的Lua GC self CPU。
   结论是成本分散于登记/查验/完成/恢复；不能把整帧都叫resume开销，也没有证据说只有single-flight skip是根因。
2. **scalar Ability：Lua→C++资格和prepared入口。** `LuaAbilityProjectionAccess::current`连同调用链占19.1–22.3%；
   其自身只是其中一部分。实际汇编包含Lua upvalue读取、prepared范围/关联检查、capture和valid回调。
   这定位了入口成本，但没有证明这些检查可删除，更没有把所有历史coroutine差值归到权限检查。
3. **record/typed：保护操作的粒度、栈/registry访问及分配。** 两字段输出的`LuaValue::run`连同子调用占81.1–82.0%；
   table创建路径25.1–30.2%、两次setField路径53.1–58.2%，是上述run的子集。
   CRT分配/释放self占21.3–25.3%，也包含在这些路径中。该record fixture使用自定义Lua allocator转发realloc，
   不能把这个malloc份额直接推广到所有backend。真实typed Pose的run为60.4–61.6%，shape为24.1–27.1%，
   current仅2.8–3.2%；因此把scalar入口当作所有record场景的首要问题并不成立。

这些百分比是采样CPU占比，不是预计可节省比例。某路径包含必要工作和其调用的VM函数，不能直接删除整段或相加得到收益。

## 数据质量、范围和方法

- VTune实际安装在`D:/Softwares/Intel/oneAPI/vtune/2026.3`（不是用户给出的单数Software目录），版本2026.3.0/build632627。
- Intel i7-13700KF，16核/24逻辑CPU，Windows11，无hypervisor。仅已有RelWithDebInfo产物，MSVC19.44、/O2 /Ob1。
- Hardware Event-Based Sampling实际返回失败：采样驱动已运行，但当前进程不是管理员。没有修改驱动/系统权限。
  本次使用10ms软件CPU采样和调用栈；不能给出真实IPC、cache/TLB miss、分支误预测、Top-down硬件瓶颈或内存带宽结论。
- 七个原脚本场景加record和typed，共九种工作量，各一次未采样执行与两次有效采样。
  只增加短场景的frames/outputs以取得足够样本，保留原size、seed、warmup、VM/GC与恢复预算；参数见plan/runs。
  FlowForge Update另加一次同量模块保留诊断，合计28次业务核对通过的进程，不把它替换成一侧更快的主样本。
- 采样覆盖完整进程，含创建/warmup/结束CSV输出和可能的AOT编译子进程。报告把writeCsv调用栈单列。
  C++ Update的CSV占16.4–21.3%；Lua Update约3.2%，scalar约1.2–1.8%。这些不是ScriptSystem运行时开销。
- 当前LuaJIT DLL与固定vcpkg安装DLL逐字节SHA一致；补入匹配PDB后从原始数据full re-finalize，初版导出也保留。
  PDB补齐解决了大量VM符号，未知JIT/动态代码仍存在。没有用不匹配符号覆盖未知地址。
- 原生/内联函数分两种视图。`beginConstruction`名下热lambda是其安装的capture/valid回调，不是每帧构造。
  `vector::size`等inlined条目与物理函数视图不得混加。软件报告仍以CPU秒显示，未把CPU秒除10ms伪造成独立样本数。
- 不锁频、不绑核；两次scalar采样CPU约14.0/20.0秒、Flow Update约19.9/20.5秒，额外诊断9.3秒，存在明显运行/采样扰动。
  占比只是两次观测范围，不是置信区间。采样耗时不用于性能等价、收益或历史回退比例计算。

## 代表工作量与绝对成本

以下为本轮**单次未采样**完整计时区间，不是五对资格或性能改善证据。单位成本摊销整个批量。
各有效采样均逐行核对同量业务、checksum、活动数与backlog；目标真实exit从VTune launcher日志核对为0。
record/typed另有捕获的VALUE标记和child exit记录，errors/backlog均0。其他旧CSV缺失的errors/failures仍为null，
没有拿退出0或缺失INTEGRITY当作完整错误计数。完整数值、首尾状态和参数见analysis/findings.json。

| 场景 | 每进程实际操作量 | 未采样累计 ms | ns/实际操作 | 持续backlog |
|---|---|---|---|---|
| lua-ability-10k | 150,000,000 provider_calls | 23553.838 | 157.026 | 0 |
| cpp-update-10k | 1,500,000,000 actual_new_calls | 7031.611 | 4.688 | 0 |
| lua-update-10k | 300,000,000 actual_new_calls | 14547.148 | 48.490 | 0 |
| lua-coroutine-10k | 10,000,000 actual_new_calls | 10511.607 | 1051.161 | 8000 |
| lua-event-10k | 20,000,000 actual_new_calls | 20379.470 | 1018.974 | 0 |
| flow-update-10k | 2,000,000,000 provider_calls | 9166.479 | 4.583 | 0 |
| flow-event-10k | 20,000,000 provider_calls | 12582.065 | 629.103 | 8000 |
| record | 20,000,000 outputs | 4851.619 | 242.581 | 0 |
| typed | 5,000,000 provider_calls | 8413.203 | 1682.641 | 0 |

Lua coroutine每5000帧50M Hook候选、10M实际新调用/恢复；Flow Event每10000帧100M候选、20M新调用/恢复，
二者原预算2000、持续backlog8000。Lua Event原场景实际预算10000，本轮2000帧20M新调用/恢复，backlog0。
本轮没有改变预算或靠提前取消减少业务；停止生产drain的既有资格仍指向原SR-6记录，本次没有伪造新drain验收。

## 模块覆盖与尚未解析的份额

下表主比较只用每场景两次有效采样；Core指simulation_script.dll，LuaJIT指lua51.dll。
“未知模块”是无法归属映像的CPU；“未命名函数”还包括已知模块内func@地址，二者不能相加。

| 场景 | Core | LuaJIT | 未知模块 | 未命名函数 |
|---|---|---|---|---|
| lua-ability-10k | 8.5–10.3% | 43.6–45.3% | 10.7–11.6% | 11.0–11.9% |
| cpp-update-10k | 62.0–67.2% | 0.0–0.0% | 0.0–0.0% | 0.0–0.0% |
| lua-update-10k | 10.1–11.3% | 39.1–41.3% | 26.8–26.8% | 26.8–26.9% |
| lua-coroutine-10k | 52.0–55.9% | 28.6–31.7% | 1.8–2.2% | 2.8–3.1% |
| lua-event-10k | 49.6–50.7% | 36.1–37.6% | 1.1–1.2% | 2.5–2.5% |
| flow-update-10k | 31.4–31.7% | 0.0–0.0% | 0.0–0.0% | 33.1–33.6% |
| flow-event-10k | 79.6–80.8% | 0.0–0.0% | 0.0–0.0% | 2.6–2.7% |
| record | 0.0–0.0% | 51.1–55.9% | 0.0–0.0% | 4.1–5.0% |
| typed | 2.1–3.5% | 63.6–63.6% | 0.7–1.7% | 5.6–6.6% |

Lua Update约26.8%的CPU不属于已知静态映像，调用栈落在Lua执行附近，可能包含JIT代码，但本轮未建立JIT代码地址映射，
不能把这26.8%精确分配给某个Lua函数。scalar约10.7–11.6%也保留为未知模块。
FlowForge Update原两次约33%的CPU仅定位到已删除的临时AOT模块；补采保留实际加载DLL后可细到RVA及汇编，
仍没有凭空生成C++函数名/源行。额外诊断的绝对耗时不与原两次混合。

## 源码与汇编证据

所有11项汇编导出均非空；见原始记录`assembly/manifest.json`及逐指令CPU/源行CSV。

| 位置 | 观测与具体含义 |
|---|---|
| [ScriptExecution.hpp](../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptExecution.hpp) 的waitEvent/createAwaitableRecord | waitEvent先findExecutionInstance，再eventSource→findActiveSlot；createAwaitableRecord又查execution identity。随后来源登记、awaitable关联与回滚各有自己的检查。多个查找在实际调用链上，不是根据类名推测。 |
| [ScriptInstances.hpp](../engine/domain/simulation/builtin/script/pinclude/lux/engine/simulation/script/ScriptInstances.hpp) 的eventSource，约391行 | 调用findActiveSlot后按0x188字节Mount步长访问，读取event_sources的begin/end并计算数量，再匹配scope/完整instance/layout epoch。汇编有乘法/移位与多级读取；没有硬件数据证明究竟是load miss还是算术延迟主导。 |
| [LuaScriptBackend.cpp](../engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp) 的current，2013行 | upvalue与active frame/原型layout、prepared slot/context/dispatch检查；之后captureInvocation和valid。既有standalone/失效authority及生命周期区别保留。 |
| [ScriptInstances.cpp](../engine/domain/simulation/builtin/script/src/ScriptInstances.cpp) 的bindInvocation，463行 | capture/valid lambda是热路径；回调定义所在beginConstruction名称不等于构造成本。 |
| [LuaValue.cpp](../modules/function/script/lua/src/LuaValue.cpp) 的run，64行 | 每个protected primitive准备栈、registry trampoline和Operation，再pcall；table及每个setField各走一次。两字段输出有三次受保护操作，不是只有一次provider调用。 |
| C++ Update物理invoke | 含内联ticket/身份/方法查验的入口成本明显；其inclusive还包括provider，不能把65–71%的整条路径全说成检查开销。扣除CSV后归一化会改变分母，原数值未被替换。 |
| FlowForge Update补采AOT RVA0x1020/0x1040/0x1070 | 0x1020读取prepared context/dispatch并间接call；0x1040执行值更新并调用两个provider adapter；0x1070取得host/context后调用body。明确存在调用封装层，而非通用运行时symbol查找。没有PDB，按RVA命名。 |

软件采样命中ret后的nop、函数序言或间接调用附近，不证明该条指令单独消耗该CPU份额；不据此直接删nop或盲目forceinline。

## 后续修改应如何选择（本次未实施）

| 优先级 | 可验证的窄问题 | 必须保留/验证 |
|---|---|---|
| 1：持续Event | 无用户代码介入的登记区间，是否可复用已有身份查找结果/冻结来源计数，减少重复map访问与冷Mount读取 | 完整代次、scope/layout权限、容量/背压/失败回滚、来源双向取消；不能跨用户代码缓存ACTIVE，不能扩大恢复预算 |
| 2：typed/record值操作 | 针对已支持的简单布局，操作粒度是否可减少重复registry/stack工作；先区分allocator与保护封装成本 | OOM/栈恢复/构造逆序清理、custom重入和方向规则；不得把一般拥有型临时对象机械塞进一次pcall，不增加legacy/fast双轨 |
| 3：scalar入口 | 保留初始资格与必要后验的前提下，窄访问数据/回调接口是否有可证实的重复传递 | 失效authority拒绝、同VM合法嵌套、Begin/End及普通错误后的recovery；不把占比当作删除检查许可 |

上述只是本轮证据支持的候选位置，未承诺节省幅度，也没有自动启动优化。需要进一步证明“缓存瓶颈/分支瓶颈”时，
应在管理员权限下对同一产物取得VTune硬件事件；当前会话无法给出这些硬件数据，不调整驱动或系统安全设置。

## 无效尝试与交付

硬件采样权限失败、错误场景名的空探测、四次缺失业务stdout的record/typed采样、一次cmd输出包装启动失败均保留。
严格检查发现缺少业务标记后，使用不采集Python父进程的launcher捕获真实child stdout及退出码，四次补采通过；
原四次只保留供诊断，不混入有效工作量/占比表。临时AOT文件退出删除的情况另记为符号覆盖限制，不删掉不利样本。

[新VTune原始记录与重放入口](evidence/script/vtune-2026-09-08/README.md)包含各命令、身份、原生结果、初版及full符号导出、
业务CSV、汇编和无效尝试。原SR-6归档及其哈希未改写；本次未运行新CTest/消费者资格、未改变生产/模板、未合并main或创建新阶段。
