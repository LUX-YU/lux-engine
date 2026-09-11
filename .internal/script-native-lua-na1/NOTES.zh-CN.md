# V4-NA1 实现与作者合同

实际资格源码：`4a8812e9813fe90b241dba29e3e668e4a4239b26`。生产 A：`f7d2815bdd2025ee23a7c11449def822413f58e9`。报告提交不替换上述身份。采用状态：`NOT_APPROVED_PENDING_REVIEW`。

## 一条执行链

`ScriptSystem → 原有 PreparedInvocation → 组合 facade 所准备的 CppStatic start → C++ coroutine → 原有 context.wait/context.delay → ScriptExecution + Event/Timer → 原有稳定恢复点 → CppStatic resume → context.callStep → Lua main_thread 上的同步 protected call → 原 Lua 业务 → scalar/void → C++ task 完成及销毁`。

没有新 scheduler、Ready 环或 core execution 对象。组合 facade 只占一个 LUA_SOURCE descriptor；未映射导出直接准备原 Lua 后端入口。映射失败报错，不自动回退。核心只看到一个 mount、完整 ScriptInstanceId、scope 和 ScriptBehavior。CppStatic 和 Lua 各有一个子对象，不形成第二脚本实例。

## 正式 owner 与寿命

| Owner | 本轮状态/操作 | 结束条件 |
|---|---|---|
| ScriptInstances / ScriptSystem | 原完整身份、权限、生命周期、执行区域与退休协调 | 原回收保护结束；本轮未改 |
| ScriptExecution / EventWaits / Timers / CompletionIngress | 原 C/A、来源、pin、ready、frontier、预算 | 原取消与完成协议；本轮未改 |
| NativeLuaTaskBackend::Impl | 深复制的冷期 plan，固定实例/方法槽，双子对象，companion lease，import 重排数组，步骤视图及发布代次 | 原核心任务和调用结束后销毁；不以 Lua active_continuations==0 作为组合回收依据 |
| CppStatic | 编译器 coroutine frame，已有 CoroutineContinuation、frame 限额，借用的步骤视图 | 正常完成/显式 FAILED/取消恰好销毁一次；Hook 解锁和核心配额时点保持 |
| Lua | main_thread、self/prototype、prepared 函数根、受保护同步事务 | 当前同步借用结束后恢复栈；prepared/实例退休释放；不制造任务 thread |

实例创建为：验证两制品 kind/content/contract、导出形状及 import 子集 → 预留稳定数组 → Lua child → prepared steps → CppStatic child → 发布。局部失败按逆序回滚。Lua 原制品 lease 由核心持有；companion lease 由 facade 持有，后者在 native/steps/Lua child 结束后释放。重新物化复用固定槽及已有 vector backing，不在发布后增长步骤存储。

退休为：原核心撤权及保护 → 原 execution/source 清理 → 有资格的 EndPlay → 外部 prepared 撤销 → native child → 步骤视图失效并释放步骤 → Lua child → companion lease。BeginPlay/EndPlay 仍只有核心原来的调用时机和资格。

## 调用接口

- `ScriptInstanceCreateContext::sync_steps` 默认空。新增的 public `ScriptSyncStep.hpp` 不包含 Lua 私有类型。
- `PreparedScriptSyncStep` 为只读签名和窄 BoundScriptCall；`ScriptSyncStepSetView` 携带完整实例、发布代次、behavior 和有限 current 回调。没有 writable authority/State 容器。
- `context.callStep<R(Args...)>(ordinal, ...)` 返回 `expected<R, ScriptSyncStepError>`。当前参数为既有 scalar/有限 enum/record；返回限 scalar/void。参数槽、同步返回的 expected 和结果中转只需在同步调用内存活；源码 helper 不保证编译后的物理栈放置。后续机器码审计发现 MSVC 仍将 P4 的部分 ABI 暂存放入 coroutine frame（实际 352 B），见 [P4 操作审计](P4-OPERATION-AUDIT.zh-CN.md)。需要跨等待的 record 按既有 typed 输入持有。
- 前置条件为原 owner 的有效 start/resume 调用。验证原 full instance、公有视图 publication、ordinal、签名和当前资格；用户代码后复验原 capture 和原 publication，不重新获取 ACTIVE。
- `context.fail(error)` 把 outcome 写为 FAILED 并返回 start/resume adapter，不分配 Awaitable、不入 Ready、不执行后续业务。适配器保留实际错误码并销毁一次 frame。没有改变全局异常策略。

同步 Lua 事务使用一个外层 pcall，覆盖参数、函数执行、返回转换和清理；内部 callback 自己 checkstack，未靠 caller 的额度替代 W0。自定义 converter 继续使用既有必要保护与字段时序。C++ owning 临时对象不处于可能 longjmp 穿越的位置。输出先存到局部，再通过原资格重验才向调用方发布；业务已经执行后的输出失败不会假装回滚业务。

引擎 await 在同步上下文缺少 resumable Step/continuation 时于登记来源之前拒绝；普通 coroutine.yield 不跨同步 C 边界。Lua pcall 捕获普通错误后仍可继续，不设置全局错误锁存。同 VM 合法嵌套恢复准确主栈 base、执行 context 和原调用资格。

## 已登记的不兼容变化

C++ 创建 context/CppStatic frame context 新增步骤视图，因此需要重新编译消费者。Native C ABI 仍为 6，不能据此混用 A/H1/A1 的 C++ EXE/DLL。新增组合模块使用新安装组件 `simulation_script_native_lua_tasks`，旧 Lua 模块产品路径保留。

这是显式作者模式：不保证原整体 Lua coroutine 的 `coroutine.running()` 身份、挂起调试栈、任意 Lua 局部对象跨等待存活。原同步步骤内的 Lua 运算、副作用、snapshot/late-read 与源顺序保留。没有透明 thread/table 池，也没有把 Lua 运算搬成 C++ 运算。`State→Impl/main_thread/invokePreparedSync` 的机械命名变更单列，不计收益。

## 测试和观测边界

主 fixture 使用真实生成 CppStatic task、Lua backend、Ability、Event endpoint 和 ScriptSystem。installed consumer 只读新 SDK public Config/header/library/tool，真正执行 Event→NextStep→同步步骤→结果→失败/取消/关闭。新增增量诊断把 expected step contract 写在独立安装客户端中，确保改变制品元数据时不能随之偷偷改变作者声明；三项冷拒绝以消费端有效 assert 捕获，contract 改名以真实 symbol ledger 拒绝。

统计关闭表示未采集。正式计时 CSV 的每波 errors/backlog 留空，进程结束的累计 errors、实际核心挂起/恢复数、逐实例 readback 和 shutdown 另行检查。批次时长包括任务创建、完成、销毁与必要 drain；准备耗时另外记录，不把业务恢复的每次 pcall 单独冒充完整任务。

所有失败尝试保留，包括最初超 512 B 编译器 frame、错误测试时钟、安装组件漏导出、C++20 expected 推导错误和复用构建槽的旧架构探针缓存。编译器超限通过移出同步临时值修正，没有扩大 frame 限额。旧构建探针首轮负例不计有效通过，使用 --fresh 后完整重跑。

## 热路径审计的后续实施

32 项审计已有实际处理，见 [修改与合同](HOTPATH-CHANGES.zh-CN.md)、[最终资格及成本](HOTPATH-RESULT.zh-CN.md)和 [逐项结果](HOTPATH-OPERATIONS.csv)。本次资格源码为 21d10601；原 NA1/V4 数据和采用待审阅状态不改。

## 第二轮等待与结果交接优化

后续六项修改、联合验证和当前对照已完成，见 [修改合同](NEXT-CHANGES.zh-CN.md)、[结果与限制](NEXT-RESULT.zh-CN.md)和 [完整机器可读结果](next-result.json)。最终资格源码为 7f201916，生产对照为 21d10601。Event 路径有实测收益，NextStep/Lua scalar 分别保留 +1.95%/+2.02% 的未解释小幅新增成本；采用仍待独立审阅，不将历史数据重标为本轮测量。
