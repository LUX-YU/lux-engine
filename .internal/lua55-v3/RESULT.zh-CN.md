# Lua55 v3 实施结果

状态：**IMPLEMENTED_AWAITING_REVIEW**。R1–R4 已实际完成，R5 按浅调用事实不采用，R6 联合资格已完成。
五条同量业务均为 3/3 对更快；不授予性能等价，不把整体收益分别重复归给各个改动。

[机器可读结果](result.json) · [原始证据与下载入口](../evidence/script/lua55-v3/INDEX.md) ·
[v2 已交付结果](../lua55-v2/RESULT.zh-CN.md)。本报告是登记提交，不是新的测试源码。

## 1. 实际身份和边界

| 项目 | 身份 |
|---|---|
| 开工参考 | `10a70e80e6e2879ecb09f14ffc083516d0666752` |
| 实际 v2 性能镜像 | `a6f16d6de6a67f6a4422553d31b42c1ac2b3e4c0`；49 项、255118963 B 与旧 manifest 全部匹配 |
| 最终 clean Engine 资格 | `e06208deb29b9b1de52fe49a129091f693bb7e08` |
| VM 执行测试的源码 | `5985cc4d49fba8aecfcb40fa9bdb70ea0204f55a`；至最终候选仅 VM README 变化，生产/测试/CMake 输入逐项核对一致 |
| VM | Lua 5.5.1，`lux-lua55-v3-r2`，扩展 revision 4；正式 diagnostics OFF |
| 官方 tar SHA-256 | `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce` |
| 活动 patch SHA-256 | `7c1390a296e9bf01d3f28f04f78722817447883eb56f2cfcfe50c8f27047281b` |
| 扩展头 SHA-256 | `720228b86990ae5e1f726dd61d1f67331d99c00f3409a290f42c4dd0487e6937` |
| 已安装依赖 | lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`，toolset `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f` |
| 编译/执行 | Windows x64，MSVC 19.44.35228.0，RelWithDebInfo、/MD；13700KF，timing affinity mask 16、worker 0 |

开发使用 `build/RelWithDebInfo/s5/source` 隔离克隆；实际资格使用独立 clean
`script-region-opt/final-source`。复用 Developer/Toolchain 槽位 `o/w/d`、`o/w/t`；
安装隔离至 `install/o/v3/{sdk,tools,lua55}`。baseline 镜像保留，未创建每子项完整构建树。
原始 main 的 HEAD、七项未知改动和各文件 hash 均与开工一致；未操作 main 分支。
198 项固定依赖文件及六个已有额外 LIB 与原记录匹配，未把源仓 HEAD 当安装身份。

官方 tar 和旧 r2 patch 原件保留。活动组合 patch 修改的 VM 文件在
`cmake/dependencies/lua55/patches/lua55-v3-r2.patch`（ldo.c、lstate.c、lstate.h、lvm.c），逐文件输入 hash 在包内
`vm-source-manifest.json`、`vm-patched-inputs.json`；DLL/PDB/资产、安装文件分别在 `final-image-manifest.json`、
`installed-final-manifest.json`。INC、上游 GC 参数、Native ABI 6、已有双方编译/IPO设置不变。
没有恢复 Lua54/JIT、没有 stack 合并、跨 lua_State 保护合并、线程池或额外 scheduler。

## 2. 真正删除的工作和新链路

| 子项 | 状态 | 正式路径变化 |
|---|---|---|
| R0 | 完成 | 保留并核对 v2 镜像，执行一次当前 Event 软件采样 |
| R1 | 完成 | 删除逐块 `LuaAllocationCache`；64 KiB 页供给小对象，复用已被 Lua 释放的 slot；正式回调实例化 `Track=false` |
| R2 | 完成 | builtin publication 冷期授予 local starter；reserve 本地 Awaitable → Timer 来源 → Execution 完成，跳过创建外部 completion/registration/lease |
| R3 | 完成 | 私有 LX 内两额外 CI；浅调用不再分别申请两个 CI，小对象仍按真实请求尺寸选 class |
| R4 | 完成 | 默认 plain 树转为具体 `LuaValueCodec<T>::readPlain/writePlain`，删除热路径逐字段 plan/member 间接调用 |
| R5 | NOT_SELECTED_SHALLOW_PATH | 未建立证书/摘要，也没有遗留无效产品开关 |

R1 实现在 [LuaPageAllocator.hpp](../../modules/function/script/lua/pinclude/lux/engine/function/script/lua/LuaPageAllocator.hpp)。
八个 payload class 为 32/64/128/256/512/736/1536/4096 B，每块含对齐 owner header；
空页可换 class，活动页不整体释放。大块走 direct allocation；页请求失败只尝试一次 direct fallback。
原位置 resize 在容量内完成，增长失败保留旧块；ptr=null 时不把 Lua osize type tag 当旧字节数。
`clear()` 只回收空页，不能用 bulk free 掩盖存活对象。可选累计/peak 计数在创建时选定的模板回调中消失；
快照仅在显式诊断时走页链。真实机器码确认 `allocate<0>` 调用 `acquire<0>/release<0>`，没有 stats 区更新；
`allocate<1>` 单独存在用于诊断。仍有容量、页链、OOM 和必要资源状态操作。

配置 `cache_bytes` 已删除，以 `idle_page_budget_bytes` 表达新含义，无兼容别名。
VM leaf/CI 计数由全局可选诊断读取，正式宏关闭；原 per-thread leaf 计数和 continuation 销毁读取已移除。
真实 DLL 反汇编中 `destroyLuaContinuation` 保留 root/线程/实例清理但无 leaf stats 调用，
两个诊断 API 正式版本返回未观察标志。不能把这两个 API 输出的原始零值当“没有 yield”。

R2 入口是 [ScriptLocalAsync.hpp](../../engine/domain/simulation/scripting/core/include/lux/engine/simulation/scripting/ScriptLocalAsync.hpp)、
Preparer、ScriptTimers 与 Execution 的窄 local 接口。授予检查覆盖 builtin provider context、dispatch、
schema 与实际 publication 的 method 数组身份。NextStep、seconds、simulationSeconds 可 local；
realSeconds、同名自定义 provider、未知 publication 保留真实外部能力通路。
Lua projection、CppStatic 生成模板、Native ABI6 adapter 和实际 FlowForge 编译模块均接入；
不是只加一个 benchmark 特例。外部 starter 用户回调仍实际执行。

局部等待与外部等待仍共用同一逻辑 Awaitable 容量。先 reserve Awaitable，再验证 duration/Timer容量，
失败原样回滚；负例覆盖容量不足与非法参数同现时的优先级。Timer 到期先复制完整身份和必要值，
不跨 Execution callback 保留 moving SlotMap 指针；来源取消/执行取消双向摘链，原时点归还逻辑槽位。
ResumeRing 满保留来源重试，原 deadline、step、frontier、每次 pop 后接纳、预算与稳定点均不改。
真实同容量 local/external 测试分别得到 capability 构造 0/6；安装的 CppStatic 两条链也断言 0。
这些是功能计数；timing CSV 的 capability 字段并非所有场景都填写，未拿其零值证明性能构成。

R3 的 LX 从 v2 240 B 变为 360 B，Lua state 216 B、CI 64 B、初始单独 stack 720 B。
LX 因而进入 512 B class；不会把少申请两 CI 记成“没有 thread 创建”。inline 位图在 extend/free/shrink/reset/close
均处理，inline 指针不传 luaM_free；动态深度仍正常分配。threadsize 为 LX + linked heap CI + separate stack，
nci 包括 inline 时扣掉其已包含字节。独立合同的浅调用 linked=2、inline=2、heap=0；
深度 0/1/2/3/32、两 thread、保留身份、GC shrink、reset、thread/stack/heap-CI 三层 OOM 与恢复、
全部关闭后逻辑字节为 0 均通过。

R4 在 [LuaValue.hpp](../../modules/function/script/lua/include/lux/engine/function/script/lua/LuaValue.hpp) 用现有模板展开。
真实 `ValuePose{key, velocity{x,y}, mode}` 的机器码直接读取成员并调用固定 ValueVelocity codec，
不存在逐字段 `field.plan()/field.member()` 的动态解释循环。具体 child reader 和 DLL 窄 Lua helper 仍有调用，
没有声称全内联或穿透 DLL LTO。shape/key/方向与 representation 仍冷期准备。
custom/nontrivial converter 继续同一 Codec 规则，按原字段时点执行，不预读后续字段。
protected callback 仅平凡非拥有局部；typed owning 值在外层构造/逆序销毁。
原始资格与转换后重验、strict raw shape、enum/range/缺失字段路径、const 构造、错误 recovery、
前字段 allocator/converter 改后字段，以及被 Lua 保留的独立结果 table 均有真实断言。

## 3. 正确性、消费者和失败闭环

| 实际最终资格 | 结果 | 包内原始入口 |
|---|---|---|
| Developer `all -j 4 -- -k 0` / 二轮无工作 / CTest | 通过；125/125 | qualified-d-* |
| Toolchain 同上 | 通过；109/109 | qualified-t-* |
| Lua55 正式 smoke/basic/leaf/CallInfo 合同 | 4/4 | final-vm-tests.log |
| 独立 VM diagnostics ON smoke/leaf/CallInfo | 3/3；不安装为正式产物 | diagnostic-vm-tests.log |
| 当前 SDK 全部消费者 | 15/15 | installed/consumers.json 与各 consumer 日志 |
| 迁址 values provider/结果/关闭纵向链及 Lua packager | 2/2 | relocated/results.json |
| 原生成增量/负例 | 13 项符合预期；包含预期拒绝 | value-incremental/probes.json |
| 公共 module 两头 × 三前缀同步 | 6/6 一致 | public-header-sync-audit.json |

最终干净源码验证通过，构建/测试/采样/计时均串行。新 `script_lua_page_allocator_test` 替代旧 cache 实现测试：
20,000 次随机操作×127 slots、0/16 MiB×诊断 on/off、整页/单存活块、fallback、溢出/失败保留、
真实 1,000 coroutine 与 VM 关闭平衡。引擎原生命周期/身份/取消/重入/pin/Event/预算/装配断言保留。

R2 新断言映射：`testLocalTimerCapacityAndReuse` 保留原 awaitable/Timer 容量失败；
`testLocalTimerRetirementReuse` 在另一个实例继续等待时反复重建 32 次，33 次准确清理且不提前恢复；
原队列满/迟到外部完成测试继续执行。Scene Lua 的真实 eager custom completion 仍构造一次外部能力，
其后 local NextStep/delay 不新增能力；Physics FlowForge 新真实 NextStep→query→capture 有逐 step 与释放计数；
CppStatic custom provider 验证实际 starter 增量=1。旧业务断言未改预算、未加 step。

联合阶段捕获一个本轮真实缺陷：新 C++ async lambda 按值捕获 method view 令 sequence frame 达 528 B，
超过既有 512 B 容量，实际为 calls=64、suspensions=0、errors=64。
改为生成函数内 `static constexpr` method identity，借用稳定不可变 descriptor 后实物 frame 为 384 B，
恢复原 64 次挂起/64 次恢复，512 B 容量不动。`sequence-diagnostic-*`、
`sequence-frame-before/after.log`、`sequence-frame-comparison.json` 保留修前/修后证据。

其余未成功尝试也保留：两个窄 target 选错 profile、格式门禁、custom counter 初版错误地检查全局1而非本次增量1。
均在最终资格前修正；不是有效性能试验。30 个正式计时进程无无效运行，没有选最快结果补表。

首次迁址尝试 Windows 拒绝重命名被占用的开发克隆；finally 原子恢复已隐藏目录，没有强制处理。
随后实际资格 clone、两个构建树和原 SDK/tools/VM 路径隐藏时，两条链重新生成、编译、运行、无工作全部通过。
开发克隆仍可访问是限制；本轮不能声称机器上所有 source 都不可访问。
实际消费路径无测试私有头或旧 build/generated，完整不可用路径列表在 `relocated/unavailable-paths.json`。

## 4. 完整同量成本

每条腿三对独立进程 AB/BA/AB，共 30 个有效进程。seed=1592598566、affinity=16、worker=0，
Event/NextStep/scalar 为 10,000 实例、1,000 warmup、2,000 measured，恢复预算 10,000。
Event/NextStep 单位包括开始、挂起、来源完成和恢复，不能把整周期耗时叫 resumeOne 耗时。
scalar 为 v2 原一次 provider/实例；record 为原 ValuePose 嵌套双向 Ability，1,000 warmup 后 1,000,000 次。
Physics 为原混合业务，300 warmup、2,000 measured，表内单位为整帧。

| 业务单位 | v2 ns/单位（各侧中位） | v3 ns/单位（各侧中位） | 三对耗时变化 | 配对中位变化 |
|---|---:|---:|---|---:|
| Event 完整周期 | 1081.996 | 744.885 | -30.92% / -25.17% / -34.96% | -30.92% |
| NextStep 完整周期 | 1299.050 | 741.111 | -38.25% / -43.83% / -38.39% | -38.39% |
| scalar Ability 一次调用 | 175.827 | 146.689 | -16.13% / -16.25% / -18.05% | -16.25% |
| 原嵌套 record 一次调用 | 1367.411 | 1330.713 | -7.71% / -2.34% / -1.57% | -2.34% |
| Physics 混合整帧 | 3528949.850 | 2910363.850 | -21.73% / -15.89% / -17.53% | -17.53% |

百分比先逐对以 v2 为分母计算，再取配对中位；不是两列独立中位的比值。
record 三对变化范围较大且收益小，保留结果，不宣称确定的 7.7% 收益。

| 场景 | v2 三次批量秒 | v3 三次批量秒 |
|---|---|---|
| Event 完整周期 | 21.639919 / 19.907671 / 22.165266 | 14.949384 / 14.897708 / 14.417122 |
| NextStep 完整周期 | 23.901984 / 26.389468 / 25.980998 | 14.758625 / 14.822214 / 16.007384 |
| scalar Ability 一次调用 | 3.498072 / 3.516535 / 3.557848 | 2.933772 / 2.944972 / 2.915789 |
| 原嵌套 record 一次调用 | 1.367411 / 1.362633 / 1.374986 | 1.261980 / 1.330713 / 1.353382 |
| Physics 混合整帧 | 7.170109 / 7.013344 / 7.057900 | 5.612113 / 5.898868 / 5.820728 |

Event/NextStep/scalar 每次 ROI 都有 20,000,000 次完整操作；逐 batch 业务计数两侧相等。
Event 另在计时外逐实例读取，含 warmup 完成 30,000,000 次，每实例 93001，checksum=930010000；
oracle 的额外 10,000 次调用不计入 ROI。record 核验 provider=count+1000、字段业务结果和关闭。
Physics 保留真实 query、script calls、next-step waits/checksum，各帧两侧一致；不是用人为 operation multiplier 折算。
所有五条腿的 runtime 累计错误/关闭可观察范围为 0，最终 backlog=0。
NextStep/scalar/Physics 沿用各自原可观察范围，未补造 Event 式全实例读回；局部错误字段不等于证明不存在所有外部错误。

下表为三次 run 各自逐 batch 分位数的中位，Event/NextStep/scalar 是 10,000 实例批次，Physics 是帧。
record 只有整体区间，没有 p50/p95/p99；没有采集逐 continuation latency，不用 batch 尾部冒充恢复延迟。

计时场景的 VM accounting 关闭。Physics 的 EXE-local allocation 计数确实开启且为0，
只覆盖该 EXE 的 operator new；其它 timing 的未开启分配字段为未采集，不能拿CSV原始零值代替全DLL/VM分配结果。

| 场景 | v2 p50 / p95 / p99（ms） | v3 p50 / p95 / p99（ms） |
|---|---|---|
| Event 完整周期 | 10.277 / 15.894 / 20.406 | 7.220 / 10.671 / 13.719 |
| NextStep 完整周期 | 12.417 / 18.676 / 24.773 | 7.318 / 10.395 / 13.448 |
| scalar Ability 一次调用 | 1.614 / 2.448 / 3.034 | 1.311 / 2.147 / 2.775 |
| Physics 混合整帧 | 4.085 / 5.688 / 6.622 | 3.473 / 4.753 / 5.758 |

## 5. 独立内存与供给记录

两侧 Event 各一次 accounting ON，独立于 timing。进程峰值按 Windows PeakWorkingSetSize/PeakPagefileUsage
观察，包含启动、warmup、业务和关闭，不能和单独 Lua requested live 相加。
以下热段逻辑量用两侧相同 CSV 区间：measured 第一帧后到末帧，19,990,000 周期；
没有假装 v2 旧二进制能输出新增 warmed 快照。v3 完整 20,000,000 周期 ROI 快照另列。

| 量与范围 | v2 | v3 |
|---|---:|---:|
| 热段 Lua 逻辑新分配 / 周期 | 4.0 | 2.0 |
| 热段逻辑请求字节 / 周期 | 1088.0 | 1080.0 |
| 全运行 lower-heap allocation 调用 | 44791478 | 193204 |
| 全运行 lower-heap free 调用 | 44662804 | 192626 |
| 全运行 Lua requested live 峰值（B） | 47511086 | 47183630 |
| 进程峰值 working set（B） | 113180672 | 109477888 |
| 进程峰值 private commit（B） | 114176000 | 108322816 |
| 运行时关闭后、VM 关闭前 requested live（B） | 18264558 | 28984550 |
| 完整新 thread / resume / release | 30000000 / 30000000 / 30000000 | 30000000 / 30000000 / 30000000 |
| Event leaf / standard 计数 | 30000000 / 0 | 未采集（正式 VM 关闭计数） |
| Event heap CI / inline hit | 未采集 | 未采集；独立 VM 合同另有 38 / 20 |
| local ingress open / registration 的逐事件计数 | 未采集 | 未采集；真实 local 测试证明 capability=0 |

v3 warmed ROI 实际新逻辑分配 40,000,000 次、realloc 0；
下层 heap allocation 128,270、free 128,441，均为页 supply/release，direct block 增量 0。
这是 heap 函数调用，不是 OS syscall。正式产品仍创建新 thread 和返回 table，30,000,000 次线程创建/恢复/释放均未减少。
少掉每周期两个 CI 逻辑分配，LX 变大 120 B、两个 CI 共少128 B，热段请求净差为每周期8 B；
R1 与 R3 不能把同一 malloc 消失分别重复记收益。

| v3 页供给量（B，除计数） | warm 后 | measured 结束 |
|---|---:|---:|
| requested_live | 39784550 | 28984550 |
| active | 48955392 | 36175872 |
| idle | 0 | 1572864 |
| pinned_free | 343904 | 340224 |
| rounding | 7536370 | 5856370 |
| metadata | 1528032 | 1232192 |
| large | 237528 | 237528 |
| large_requested | 237464 | 237464 |
| page_alloc | 64927 | 193197 |
| page_free | 64180 | 192621 |
| direct_alloc | 7 | 7 |
| direct_free | 5 | 5 |

活动页恒等式已核验：active = requested_live - large_requested + rounding + pinned_free + metadata。
结束时 pinned free 为 0.324 MiB，rounding 5.585 MiB，
活动页 34.500 MiB，空页 1.500 MiB，direct backing 0.227 MiB。
空页峰值为16 MiB，但这个预算不覆盖活动页、pinned slack、取整、大块或总 VM 内存。
v2 的16 MiB限制是旧逐块 free cache retained 字节，口径不同，不据相同配置数字声称同内存。

本次 v3 进程峰值 working set 比 v2 -3.531 MiB，
private commit -5.582 MiB；只是此 workload 此次独立观察。
VM_FINAL 的 requested live 反而较高，两侧 GC 时点/存活集合未强制对齐，该差异未单独归因；它在 ScriptSystem 关闭后、VM 关闭前，
不能拿它声称泄漏或全VM已为零。真实 VM 关闭平衡由单独功能合同验证，不靠最终 bulk free 掩盖。
新页/direct 口径在 v2 未采集；正式 v3 leaf/CI 计数关闭，在解释后的 result.json 中为 null，
旧日志和诊断日志原样保留。独立 VM 合同的 inline=20/heap=38 不是 Event 的累计次数。

## 6. 本轮采样、机器码和剩余限制

R0 是本轮对匹配 v2 镜像做的 Event 软件 Hotspots，含全进程启动/warmup/业务/oracle/关闭，目标 CPU约28.5 s。
self 样本：luaH_Hgetshortstr 3.137 s、malloc_base 1.869 s、sweeplist 1.585 s、luaV_execute 1.370 s、
旧 allocator resize/acquire 合计2.342 s、free_base 0.594 s、freeCI 0.448 s。
lux_leaf_eligible 仅0.047236 s（约0.17%），且实际 fixture 的等待直接调用 C Event，是浅路径，R5 不采用。
没有新最终 v3 profile，所以不能把这张分布当最终热点；没有 PMU/cache-miss/branch-miss 归因。
旧26.3% longjmp比例不适用于本轮；本次整个 VCRUNTIME140 模块样本约0.152 s。

真实 PDB/优化 DLL/EXE 的机器码保留在 `machine-code/`：allocator track 两实例、销毁路径、
ValuePose typed双向转换；CI 两槽与GC记账由VM合同直接观察。保留 memcpy、Lua API helper、校验和清理所需调用，
没有全局 forceinline、单侧 LTO/ISA、GC 参数改选或减少业务工作。

历史 scalar/coroutine/record 成本债务继续引用 v2/SR报告；本轮是 v2→v3 的新证据，
不认定残余都是必要安全成本。当前剩余 Lua bytecode、table 操作/对象创建、GC、stack、typed child/helper调用仍在；
收益归因仅到整体结构组合，未做全组合消融。五腿均3/3更快，未观察到需要消融确认的整体回退，各子项收益不单独分摊。
只验证 Windows/MSVC/RelWithDebInfo 的上述配置；Linux、Android、其它compiler、完整上游内部C库与游戏项目未验证。

## 7. §9 最终检查回答

1. 没把减少 malloc 说成没有对象创建；两侧线程数同为三千万。
2. local reserve 不创建 external completion，功能/安装断言 capability=0；不是仅优化到期函数。
3. 授权来自真实 publication/context/dispatch/schema/数组身份，不能只凭方法名。
4. custom starter 保留且本次增量=1；realSeconds 和外部 provider 仍有实际通路。
5. inline CI 独立位图识别，free/shrink/reset/close 全覆盖，不传 luaM_free。
6. threadsize 扣除 nci 中 inline 份额，LX及stack/heapCI只记一次，关闭账目为零。
7. allocator正式 Track=false、VM诊断宏OFF；可选累计/peak工作不在正式每请求路径。
8. 16 MiB只限空页；实际 pinned/rounding/metadata/direct分别列出，没称同内存。
9. typed callback 的真实 ValuePose机器码没有动态member/plan遍历，固定child/helper调用仍诚实列出。
10. 未跨callback保留moving Timer指针、未跨用户代码缓存ACTIVE、未预读后字段；合法重入负例保持。
11. 五腿严格沿用 v2原业务单位、warmup和操作数，没有把完整周期改名为纯resume成本。
12. 未采集字段为null，失败尝试单列；R5为NOT_SELECTED，不假装实现或验证。

完成后停在 v3统一审阅，不合并main、不发tag、不冻结框架，不自动进入stack合并或下一优化阶段。
