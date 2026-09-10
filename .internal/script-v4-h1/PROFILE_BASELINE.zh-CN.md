# V4-H1 当前 V4 软件采样


实际 A 源码 `f7d2815bdd2025ee23a7c11449def822413f58e9`。五份本轮采样均从保留 V4 镜像启动，target exit=0 且业务 oracle 通过。

采样使用 VTune 2026.3 User-mode sampling，10ms，采集调用栈。固定 EXE 没有可用 ITT ROI 接口，范围为完整目标进程（包括准备、预热与清理），**不是精确业务循环 ROI**。没有 PMU 数据，不推断 cache miss/branch miss。

CPU self 是函数自身采样，top-down 是 inclusive 调用树；两者不相加。不得把带采样耗时作为普通性能数据。


## event


目标 CPU 合计 18.497161s。原始记录：归档 `baseline-profile/event/`；本机 `E:\SyncForder\CodeRepos\build\RelWithDebInfo\script-v4-h1\baseline-profile\event`。

| 函数 | self秒 | 占CPU |
|---|---:|---:|
| luaH_Hgetshortstr | 0.784183 | 4.24% |
| lux::script::lua::LuaPageAllocator::release<0> | 0.615056 | 3.33% |
| traversestrongtable | 0.593914 | 3.21% |
| luaV_execute | 0.548691 | 2.97% |
| lux::simulation::script::detail::ScriptInstances::eventSource | 0.547344 | 2.96% |
| lux::simulation::script::detail::ScriptExecution::resumeOne | 0.476140 | 2.57% |
| stack_init | 0.403720 | 2.18% |
| lux::simulation::script::LuaScriptBackend::State::resumeLuaContinuation | 0.336941 | 1.82% |


## flowforge-event


目标 CPU 合计 3.532072s。原始记录：归档 `baseline-profile/flowforge-event/`；本机 `E:\SyncForder\CodeRepos\build\RelWithDebInfo\script-v4-h1\baseline-profile\flowforge-event`。

| 函数 | self秒 | 占CPU |
|---|---:|---:|
| lux::simulation::script::detail::ScriptInstances::eventSource | 0.188265 | 5.33% |
| lux::simulation::script::NativeScriptBackend::State::invokePreparedStep | 0.146893 | 4.16% |
| lux::simulation::script::detail::ScriptEventWaits::registerWait | 0.141226 | 4.00% |
| lux::simulation::script::detail::ScriptExecution::resumeOne | 0.134216 | 3.80% |
| lux::simulation::script::detail::NativeFrameStorage::acquire | 0.119525 | 3.38% |
| lux::simulation::script::PreparedResumeType::valid | 0.090963 | 2.58% |
| std::optional<lux::cxx::SlotKey<lux::simulation::script::detail::ScriptExecution::AwaitableTag,unsigned int,unsigned int> >::{ctor} | 0.080290 | 2.27% |
| lux::simulation::script::detail::ScriptExecution::reserveAwaitable | 0.080106 | 2.27% |


## scalar


目标 CPU 合计 3.632903s。原始记录：归档 `baseline-profile/scalar/`；本机 `E:\SyncForder\CodeRepos\build\RelWithDebInfo\script-v4-h1\baseline-profile\scalar`。

| 函数 | self秒 | 占CPU |
|---|---:|---:|
| luaH_Hgetshortstr | 0.446918 | 12.30% |
| luaV_execute | 0.379139 | 10.44% |
| index2value | 0.223629 | 6.16% |
| lux::simulation::script::LuaScriptBackend::State::invoke | 0.175270 | 4.82% |
| lux::simulation::script::ScriptBehavior::captureInvocation | 0.129006 | 3.55% |
| luaD_precall | 0.127949 | 3.52% |
| luxLuaBoundaryEntry | 0.113438 | 3.12% |
| lua_rawgeti | 0.112082 | 3.09% |


## record


目标 CPU 合计 1.147428s。原始记录：归档 `baseline-profile/record/`；本机 `E:\SyncForder\CodeRepos\build\RelWithDebInfo\script-v4-h1\baseline-profile\record`。

| 函数 | self秒 | 占CPU |
|---|---:|---:|
| index2value | 0.082600 | 7.20% |
| mainpositionTV | 0.070211 | 6.12% |
| luaH_Hgetshortstr | 0.058460 | 5.09% |
| luaV_execute | 0.058137 | 5.07% |
| equalkey | 0.056961 | 4.96% |
| lua_gettop | 0.042980 | 3.75% |
| lua_settop | 0.039801 | 3.47% |
| lua_rawgeti | 0.029854 | 2.60% |


## cpp-sequence


目标 CPU 合计 2.893783s。原始记录：归档 `baseline-profile/cpp-sequence/`；本机 `E:\SyncForder\CodeRepos\build\RelWithDebInfo\script-v4-h1\baseline-profile\cpp-sequence`。

| 函数 | self秒 | 占CPU |
|---|---:|---:|
| lux::simulation::script::detail::ScriptTimers::swapHeap | 0.152715 | 5.28% |
| lux::simulation::script::detail::ScriptTimers::registerWait | 0.115852 | 4.00% |
| lux::simulation::script::detail::ScriptExecution::resumeOne | 0.101061 | 3.49% |
| lux::cxx::Fnv1a64::hash | 0.098047 | 3.39% |
| lux::simulation::script::CppStaticScriptBackend::State::resolveEvent | 0.088917 | 3.07% |
| lux::simulation::script::ScriptOwnedBytes::{ctor} | 0.071273 | 2.46% |
| lux::simulation::script::detail::ScriptTimers::earlier | 0.070918 | 2.45% |
| lux::simulation::script::detail::ScriptTimers::cancel | 0.068183 | 2.36% |


## 解释与限制


Lua Event 仍包含 Lua table 读取、GC/页释放、字节码执行、实例 Event 来源解析和 resumeOne；不能说检查占据大多数时间。FlowForge 可观察到 prepared invoke、来源登记、frame 分配、类型验证；H1 仅删除其中冷期可证明的重复形状判断。

ValuePose 只有约1.1秒 CPU，软件样本粗；不为微小百分比作精确归因。FlowForge 目标退出后编译器 vctip 子进程延长 collector 生命周期，收到完整 target oracle 后停止采集；collector elapsed 不作为业务耗时。

五次 bottom-up 报表命令不受本版本支持，失败输出保留；有效导出为 hotspots、top-down 和 summary。没有重新采候选热点，候选使用独立无采样对照与匹配 PDB 机器码。

PDB 的 S_GPROC/S_LPROC RVA+code size 界定函数。objdump 文本附近的导出别名不是函数归属依据。机器码索引包含实际 DLL/PDB hash、RVA、代码字节、分支/调用/除法数量。
