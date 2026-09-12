# Lua Event 协程逐阶段 VTune 调查（2026-09-08）

本轮完成实际采样、分阶段计时、VM 分配诊断和优化产物反汇编；没有修改生产运行时、生成模板或脚本。
调查对象是当前 Lua Event **完整执行周期**，不是孤立 `lua_resume()`。

结论：当前负载的首要成本是反复启动新执行及登记等待，其次是恢复协议和清理。
事件已经按通知恢复；本次没有发现逐帧扫描所有挂起 continuation 的路径。
约 60% 的采样 CPU 在启动/登记，14% 在事件交付，26% 在恢复/清理。
新 thread 创建入口占完整周期约 18%，其中触发的 GC 约 14%；后者已经包含在前者中。
不能把完整周期约 570～580 ns/次称为 Lua 恢复调用成本。

## 1. 身份与测量边界

- 分析分支起点：`codex/s6-deep-optimization@6dcbe6562c0effa5ca84d88f0d205086e95aaba0`。
- 实际编译源码：独立 clean clone `bc2dbfe2270a4b6777207deceb1ad33b78a02920`；已核对与分析分支的
  `engine/`、`modules/` 无差异。本报告的文档/诊断提交不是新的生产资格源码。
- 复用 `build/RelWithDebInfo/o/w/d` Developer 构建槽位；全部使用 RelWithDebInfo、
  MSVC 19.44.35228.0、`/O2 /Ob1 /Zi /MD`，没有单侧 LTO、ISA 或编译规则修改。
- 编译依赖来自既有 `install/q2/c` 与 `install/q2/toolset`。其既有固定身份分别为
  `3100f54d0743c5ed94a4ccf5943df04e933de255` 和 `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`。
  本轮逐项复核安装文件前后哈希，并将实际使用的 SlotMap、StableSlotMap、expected 安装头与
  `3100f54d` 内容比较通过；不将此窄检查扩大为重新完成全部依赖安装资格。
- 同级依赖**源码工作区**实际已是 `lux-cxx@7716c087`、`toolset@961c63ed`，均未改动，
  没有用这些源码升级上述安装依赖。原始 main 的七个未知修改/文件前后哈希保持不变。
- 使用真实 `lua_runtime_benchmark_fixture.lxsa`，输入是现有 `.lua`、描述和 symbols 文件，
  路径及哈希见原始 `identity-final.json`；VM 为 **LuaJIT 2.1.1771261233，JIT available/enabled 均为 1**。
  不能从 enabled 推导每段跨 C/yield 的代码都已被 JIT 编译。
- 开始时九项 EXE/DLL/PDB 与上轮资格哈希一致。全量 `target all` 有 13 项工作，重链了
  同一 clean 源码的 core DLL/PDB，因此**本轮计时使用新的实际产物哈希**：
  `lux_engine_simulation_script.dll = 6a5290293e53e52fed5116cfc6fb053f2f83d245b18e3420ffa3d373d154ebdf`。
  PDB 为 `96566c04bc11b705854c653c12ee28a42eec79537d3db109ec0ef5dd2173b2a6`。
  没有把上轮二进制身份照搬到本轮。实际图像保留在本地证据目录的 `images/`。

VTune 为 `D:/Softwares/Intel/oneAPI/vtune/2026.3/bin64/vtune.exe`。
硬件采样探针再次报告采样驱动/管理员条件不足，故使用 software Hotspots、调用栈与 ITT task。
本轮没有 PMU、cache miss、分支误预测或 IPC 结论；指令上的采样不能解释该指令本身的精确延迟。

## 2. 实际执行的负载和观察方法

脚本没有改写，实际方法为：

```lua
function BenchmarkBehavior:wait_event()
    local payload = lux.Event.Benchmark.event()
    self.value = self.value + payload
end
```

每批 10,000 个 Entity 实例，Hook 启动 10,000 次方法；一个 broadcast occurrence 交付
int32 值 31；advance 的模拟 duration 为 0，再执行既有 stable point。方法恢复、写入 self 后返回。
下批再创建新的执行。固定 seed `1592598566`，executor workers=0，实际一个执行线程，
有效恢复预算为 10,000。既有 Lua Event harness 本身使用 size 作为预算，未通过通用参数扩大预算。

诊断 TU 复用 clean clone 的原始 benchmark harness 和已生成对象，链接实际生产 DLL。
四个批次操作外各有 QPC/steady_clock 计时；只在 VTune 运行启用 ITT，未在单次 coroutine 内插时间戳。
每次操作前后读取已有统计，在计时区间外验证状态。所有测量、构建与测试串行。

| 操作结束 | continuation | awaitable | waiter | ready backlog |
|---|---:|---:|---:|---:|
| 启动并登记 | 10,000 | 10,000 | 10,000 | 0 |
| occurrence 交付 | 10,000 | 10,000 | 0 | 10,000 |
| advance(0) | 10,000 | 10,000 | 0 | 10,000 |
| stable point 恢复 | 0 | 0 | 0 | 0 |

每次进程退出前检查 errors、剩余执行状态及 shutdown 后的实例数；没有用空 failures 或进程退出码单独代替工作量。
本轮没有逐实例读回 Lua `self.value`，也不是 Ability provider 纵向测试：业务证据是原脚本、实际新调用、
挂起、resume、waiter visits、thread 创建/解除引用及关闭资源计数。此观察范围不应扩大。

实际运行：

- 5 组独立进程配对：原完整计时器 ↔ 分阶段诊断，交替顺序，各 warmup 1,000 批、计时 2,000 批。
  每进程计时内 20,000,000 次新调用和恢复；warmup 另有 10,000,000 次。
- 两轮有效 VTune：各 warmup 1,000 批、采样 5,000 批，即 ROI 内各 50,000,000 次新调用和恢复。
  setup、warmup、输出、shutdown 被 pause 排除；task 过滤将统计检查等区间外样本独立保留。
- 两轮 VM 分配诊断：各计量 500 批/5,000,000 周期，分配计数开启。其时间不用于性能结论。
- 一轮小规模真实 smoke；16 项受影响生命周期、Bindings、Event、continuation、Lua 测试通过。

原始计时器把一次 `appendLuaStats` 放在计时内；阶段诊断把统计读取放在区间外并增加四个计时边界。
统计访问也可能影响缓存。两者配对用于量化观察扰动，**不是优化前后比较**。

## 3. 绝对时间、配对与尾部

下表各项取五个进程相应统计量的中位数。p50/p95/p99 都是 **10,000 次工作的批次分布**，
不是单个协程延迟分布。不同阶段的分位数不能相加。

| 阶段 | 批次平均 ms | ns/实际对应操作 | 批次 p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|
| 新执行与等待登记 | 3.4613 | 346.1/新调用 | 2.9057 | 6.2187 | 7.3165 |
| occurrence 与完成交付 | 0.8050 | 80.5/被完成 waiter | 0.7739 | 0.9526 | 1.0539 |
| advance(0) | 0.0018 | 这是每批时钟操作，不是 10,000 个 timer | 0.0019 | 0.0025 | 0.0028 |
| 恢复与最终清理 | 1.4153 | 141.5/实际 resume | 1.2351 | 2.0581 | 2.2373 |

按每个周期先求四段总和，再统计：批次平均的进程中位为 **5.6869 ms（568.7 ns/完整周期）**，
批次总和 p99 的进程中位为 **9.8173 ms**。这排除了阶段间诊断语句，不能称为整个应用帧时间。
原 benchmark 的完整批次平均中位为 **5.8156 ms（581.6 ns/完整周期）**。

| 配对 | 原计时器 ms/批 | 阶段总和 ms/批 | 诊断相对差异 |
|---|---:|---:|---:|
| 0 | 5.7630 | 5.6950 | -1.18% |
| 1 | 5.8845 | 5.6319 | -4.29% |
| 2 | 5.7910 | 5.6869 | -1.80% |
| 3 | 5.8156 | 5.6640 | -2.61% |
| 4 | 5.8252 | 5.8814 | +0.96% |

其中一对差异超过 3%，因此不把阶段数字当成无扰动的纳秒真值，也不宣称新实现变快。
原始所有批次 CSV 均保留。尾部成因尚未逐批与 GC/OS 调度关联，不能把 p99 全部归因于 GC。
本轮没有逐 waiter 时间戳，所以没有单个 waiter 的恢复延迟分布；只验证交付后该安全点耗尽预算内的队列。

## 4. 调用链与实际成本

以下 CPU 百分比均来自 ITT task 过滤后的两个独立 ROI。每行是 inclusive 子树，
父项已包含子项；不得跨层相加。inline/physical 报表是同批样本的不同归属，也不能相加。
部分 noinline 包装被编译器尾调用消除，所以阶段界定用 task，不能靠包装函数名字是否出现。

### 4.1 启动和等待登记：约 60.06% / 60.20%

```text
Hook endpoint → Bindings::visitHook → Execution::invokeStep
  → LuaScriptBackend::invokePreparedStep
    → acquireContinuation（C++ backend 固定槽复用）
      → lua_newthread → [超过阈值则 lj_gc_step] → lj_state_new
      → luaL_ref（新 Lua thread 的 registry 根）
    → lua_rawgeti(function), lua_rawgeti(self)
    → ExecutionScope → resumeLuaVm（首次启动）
      → Lua 的 lux.Event.Benchmark.event 投影
      → Execution::waitEvent
        → Instances::eventSource（原身份、权限、admission）
        → EventWaits::reserve
        → Execution::reserveAwaitable（预留拥有型结果）
        → EventWaits::registerWait（route/instance 两条关联）
      → Lua yield
  → beginSuspension（核心 continuation、关联 awaitable、Hook 禁用位）
```

| 子树 | 启动阶段 CPU 占比（两轮） | 完整 ROI 占比（两轮） |
|---|---:|---:|
| `lua_newthread`，含触发的 GC | 29.98～30.62% | 18.05～18.39% |
| 其中 `lj_gc_step` | 23.35～23.54% | 14.06～14.14% |
| 其中 `lj_state_new` | 5.88～6.09% | 3.54～3.66% |
| `luaL_ref` | 5.95～6.23% | 3.57～3.75% |
| 首次 `resumeLuaVm`，含进入引擎登记等待 | 38.09～38.21% | 22.93～22.95% |
| 其中 `Execution::waitEvent` | 25.72～27.07% | 15.48～16.26% |
| 其中 `reserveAwaitable` | 7.56～9.26% | 4.55～5.56% |
| 其中 `registerWait` | 5.25～5.30% | 3.15～3.19% |
| Lua 首次返回后的 `beginSuspension` | 10.12～12.03% | 6.09～7.23% |

机器码 `assembly-newthread.csv` 明确显示阈值比较 → `lj_gc_step` → `lj_state_new`。
GC 子树中有 table/thread marking、sweep、`lj_state_free`、allocator free，乃至 VirtualFree。
所以“thread 很轻，只分配一个 C++ 描述符”不符合这条实际路径：C++ 描述符在复用，Lua VM 对象在新建。
`waitEvent` 也不是轮询；它是在首次执行中登记一个被通知唤醒的等待。

### 4.2 事件交付：约 14.25% / 14.01%

```text
EventEndpoint::consume → Bindings::eventEntry → System::dispatchEvent
  → EventWaits::claim（截断本 occurrence 之前的登记）
  → 普通 callbacks（本 fixture 没有普通 Event callback 的业务）
  → 对仍有效的 claimed waiter：Execution::completeClaimedEventWaiter
    → Instances::active + 定位 awaitable + ResultWritePin
    → payload_projection.copy（本例 int32；通用契约允许用户代码/重入）
    → 复制后重新确认实例/结果仍有效
    → eraseEventWaiter → EventWaits::cancel → detachSource
    → finishAwaitableOwner → 结果 READY → ResumeRing::push
```

| 子树 | 交付阶段 CPU 占比 | 完整 ROI 占比 |
|---|---:|---:|
| claim 遍历 | 6.04～12.38% | 0.86～1.73% |
| `completeClaimedEventWaiter` | 85.60～93.21% | 12.00～13.28% |
| 其中 `eraseEventWaiter`，含 cancel/detach | 33.76～33.91% | 4.73～4.83% |
| 其中 `Instances::active` | 21.69～21.99% | 3.08～3.09% |
| 其中 `finishAwaitableOwner` | 17.79～18.94% | 2.49～2.70% |

这里的 `cancel` 同时承担“成功交付后消耗来源”的摘链职责，不代表脚本发生取消。
`detachSource` 重新定位对应 awaitable；调用者此时已有 pin 保护的结果记录，值得研究受限的内部直接操作。
反汇编 `assembly-active.csv` 确认 `active()` 不是一个 bool load：仍经过 identity SlotMap 范围/代次、
dense slot、InvocationState 访问。两个 `active()` 分别在 payload 用户代码前后，不是可以无条件合并的相邻检查。

每批 payload 仅 40,000 bytes，结果预留在 Execution 内。该 Event 不经过外部 completion 运输队列，
不靠 Timer 唤醒；不能把 Ingress 锁或 OS 协程切换当作它的主要成本。

### 4.3 稳定点恢复及清理：约 25.58% / 25.56%

```text
既有 stable point（lifecycle → NextStep → delay → 同 frontier 外部接纳）
  → ResumeBatch::next，pop 消耗原预算
    → resumeOne
      → findExecutionInstance、continuation 身份及关联
      → takeAwaitable：拥有型结果移动到局部 outcome，摘链并回收 awaitable 槽
      → resumeAccess（已知 mount 关联）
      → backend.resume → Lua::resumeLuaContinuation
        → pushResumeValue（prepared event、结果类型/大小、推 int32）
        → ExecutionScope → resumeLuaVm
          → self.value += payload → Lua 方法返回
      → 用户代码后的 continuation/authority 重验
      → destroyContinuation
        → 核心实例链摘除、清除 method single-flight、Hook 重新可运行、回收槽
        → backend.destroy → lua_settop、luaL_unref、backend 槽复用
  → 每次 pop 后保持同 frontier 接纳
```

| 子树 | 恢复阶段 CPU 占比 | 完整 ROI 占比 |
|---|---:|---:|
| `resumeOne` | 94.97～95.37% | 24.27～24.40% |
| 其中 `takeAwaitable` | 14.18～14.70% | 3.62～3.76% |
| 其中 `resumeLuaContinuation` | 31.18～32.09% | 7.98～8.20% |
| 其中 `resumeLuaVm`（含剩余业务） | 19.23～20.20% | 4.92～5.16% |
| 其中 `pushResumeValue` | 4.71～7.26% | 1.21～1.85% |
| `destroyContinuation`，含 backend destroy | 25.88～29.34% | 6.62～7.50% |
| 其中 `destroyLuaContinuation` | 9.81～11.44% | 2.51～2.92% |
| 其中 `luaL_unref` | 4.86～5.11% | 1.24～1.31% |

`resumeLuaVm` 的样本主要出现实际 Lua 字节码，如读取 self.value 的 `lj_BC_TGETS`。
恢复阶段近 80% 的 CPU 不在这个 VM 子树中。另一方面，首次启动的 VM 子树包含引擎 waitEvent，
不能将前后两次 `resumeLuaVm` 的 inclusive 时间直接都记到 Lua 解释器。

`assembly-value-move.csv` 显示移动结果时复制类型元数据、清零 32-byte inline 缓冲、设置 spill/size，
再依据实际长度调用 `memcpy`。本例实际数据只有 4 bytes。**并没有复制整个最大 payload 容量**；
可研究的是通用对象初始化和移动的工作量。移动子树仅占完整 ROI 约 0.73～1.14%（恢复时），不是主因。
跨用户代码后重新找 continuation 是因为它可被取消/回收，不能长期借用短命记录指针代替。

## 5. 分配与 GC 的记账位置

两轮各 5,000,000 个完整周期均观察到：

| 阶段 | Lua thread 创建/解除引用 | VM allocation | VM realloc | 请求 bytes |
|---|---:|---:|---:|---:|
| 启动/登记 | 创建 5,000,000 | 10,000,000 | 0 | 2,400,000,000 |
| 交付 | 0 | 0 | 0 | 0 |
| advance | 0 | 0 | 0 | 0 |
| 恢复/清理 | 解除引用 5,000,000 | 0 | 0 | 0 |

即每个新执行 **2 次 VM 分配请求、480 bytes**，每批 20,000 次请求、4.8 MB 请求量。
如果这种负载每秒 60 批，仅按工作量换算为 288 MB/s 的 VM 请求吞吐；不是实时帧率测量。

两轮实际 VM frees 分别是 9,981,014 / 10,020,000，释放 bytes 为
2,395,443,360 / 2,404,800,000，**全部记在启动/登记阶段**。
解除 registry 引用不等于 VM 当场释放；后续创建的 GC 会扫掉旧对象，还可能扫到 warmup 的对象，
因此一轮释放量略高于该轮请求量并非计数矛盾。

这是本轮最有价值的成本归因：清理函数自身看起来不贵，不能推出生命周期回收便宜；
回收账单转移到了下一轮 `lua_newthread`。这也解释了为什么仅压缩 resume 调用包装不足以解决完整周期。

这些是 VM allocator 的请求/释放量，不是 C++ 全进程分配、OS RSS、保留堆或峰值内存。
计时/VTune 中 vm_accounting=0 的零字段表示未观察，不能被改写为“没有分配”。
没有根据这两轮推出所有 VM 分配均由 thread 的两个对象构成；归因还依赖调用栈中 `lj_state_new/stack_init`
等入口，未做逐对象地址追踪。

## 6. 有证据支持的后续优化顺序

本轮只调查。以下是有界候选，不是已获得的收益，也不证明现有全部成本属于必要安全成本。

| 优先级 | 候选与证据 | 实施难点/不能越过的边界 | 应如何验证 |
|---|---|---|---|
| 1，保持语义的 runtime 窄改 | Event 成功交付中，携带已有的固定 mount/只读 authority 关联，避免两次走身份目录；在已 pin 的结果上直接完成来源脱离，减少 cancel→detach 的回查。交付两块合计约 7.8% ROI，尚含不可删除的摘链工作 | 复制前后仍各读一次完整身份及当前权限；不能跨 copy 缓存 ACTIVE。只能由 owner 修改记录，保留 claim 与 ResultWritePin | scalar/custom payload 分开；取消、复制中退休、嵌套登记、旧代次、容量/queue 满、来源统计与释放次数 |
| 2，登记和结果布局 | `reserveAwaitable`、`beginSuspension` 共占约 10.6～12.8% ROI；压缩临时聚合、重复初始化、已定位记录的重新解析。准备期确认的 payload 形状可由不可伪造的内部证明沿合法链传递 | 公共 admission、backend 返回和自定义 copy 是不同信任边界；不能用全局开关禁用校验，不能合并七组件为一个可写 Runtime | 对照真实生成后端；完成早于关联、反复再挂起、pin 增长、失败精确回滚；采样和反汇编确认删除的实际加载/调用 |
| 3，Lua 生命周期成本 | 新 thread 与 GC 是最大单个 VM 子树，约 18% ROI；registry ref/unref 还各有成本 | 现有 coroutine identity/dead-state 契约下，脚本仍持有的旧 thread 不能用于新执行；单纯 C++ continuation 池已经存在，不能再把它包装为新优化 | 复用既有身份负例。allocator 只能回用 VM 真正释放后的内存，且池必须有界；它不能自动消除 marking 或 Lua thread 重新初始化 |
| 4，结果移动/值推入的微成本 | 4 bytes 也走通用拥有型元数据/初始化、动态 memcpy 和 prepared 结果检查 | 移到显式外层存储，或做冷期固定的 scalar 操作，都必须保留错误/类型契约和跨用户代码寿命；不可裸借已回收结果 | 先测单项，再看完整周期；不要为了约 1% 的子树制造永久双轨或大量 forceinline |

另外有一个更大但**属于脚本用法和可观察行为选择**的方向：对于本来就是常驻事件循环的逻辑，
在同一个 Lua 方法里反复 `waitEvent`，而不是每次收到一个事件便结束、下一帧再从 Hook 创建新执行。
同一次执行重复挂起已经复用同一个 thread；这可减少 thread 创建/引用和核心 continuation 的反复建销。
但它会改变执行身份、局部变量寿命和方法开始/结束时机，不能偷换为当前 fixture 的同量优化结果。
需要另列“常驻循环”与“每次新调用”两种产品负载；即使前者更快也不能冒称后者自动同幅提升。
若业务只是 onEvent 回调、根本不需跨事件保存局部状态，普通同步 Event handler 本来就是另一种更短的路径。

GC 调整也只能作为之后的独立实验：改变 pace 可能平滑尾部，不能凭空消除总 GC 工作，还可能增加保留内存。
本轮没有改 GC 参数、关闭 GC、缩减工作量、改预算、增加恢复点或撤销检查来制造差距。

对“如果完全消除某部分会怎样”只可作数量级边界：完整移除约 14% 的 GC 子树最多解释这批样本中
相应份额，不能解释剩余约 86%，更不是现有语义下可兑现的目标。当前更合理的工作是把可省的访问与
对象生命周期成本分别处理，而非继续把全部成本归到 invoke 或 resume 的函数名。

## 7. 原始证据、无效运行与限制

[证据索引](evidence/script/lua-event-phases-2026-09-08/README.md)提供原始 VTune 项目、
逐 task 调用栈、六份汇编报告、全部 CSV、身份清单、退出码和文件哈希。
[诊断实现与重放说明](diagnostics/lua-event-phases-2026-09-08/README.md)保留实际编译、运行、解析逻辑。

保留的无效/非结论试验：

- 首次 diagnostic 编译因缺少 `<iostream>` 失败；补 include 后编译通过。两个原日志保留。
- `hardware` 因采样驱动/权限条件失败，不进入结果。
- `roi-0` 在 VTune 直接启动下缺少独立 application integrity 日志，即使 CSV 工作量符合也排除。
  改由 launcher 捕获应用输出/退出码得到有效 `roi-1/2`；旧项目没有删除或冒充有效。
- noinline wrapper 的尾调用让仅按包装函数划分的报表不完整；最终只采用 task-filtered 报表。
  解析器明确处理 VTune 在 CSV 引号前输出缩进的格式，避免含逗号的 C++ 签名造成列错位。
- 采样位于短叶函数的结果波动很大，例如 claim 占比在两轮相差近一倍；报告保留区间，
  不将软件采样解释成准确执行次数、硬件停顿或纳秒级指令成本。

`cmake --build .../o/w/d --target all -j 4 -- -k 0` 通过；第二轮 `ninja: no work to do`。
受影响 CTest 16/16 通过，命令/名称见原始日志。没有生产或模板变更，因此没有再跑完整 Toolchain、
Developer、Lua54、安装/增量矩阵；本轮不产生新的联合资格声明，也不将旧结果重标为本轮执行。
Windows LuaJIT 单一 broadcast/int32、全部同批 READY、10,000 预算的结论不能直接外推到 Lua54、
不同硬件、真实多 System 竞争、targeted event、自定义 record、长时间悬挂或不同积压。

已有支持边界和性能债务继续引用
[当前直接派发报告](script-system-direct-dispatch-optimization-2026-09-08.zh-CN.md)，不复制一套账本。
生产实现保持，后续优化等待本次分析后的独立决定。
