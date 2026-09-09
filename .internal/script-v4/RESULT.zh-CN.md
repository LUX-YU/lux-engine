# Script v4 实施结果

状态：**实施和本轮资格完成，等待 v4 独立审阅；性能不等价，成本限制保留。**

最终资格源码 `f7d2815bdd2025ee23a7c11449def822413f58e9`，独立 clean clone 构建；原 v3 对照 `e06208deb29b9b1de52fe49a129091f693bb7e08`。
起始参考 `d45b7e9284b34686fac5425fed57a79d17ffb0d3` 未回退。源码、测试与资源合同见
[NOTES](NOTES.zh-CN.md)，全部实际数值与命令见 [result.json](result.json)。
唯一原始包见 [证据索引](../evidence/script/script-v4/INDEX.zh-CN.md) 或
[固定提交下载](https://raw.githubusercontent.com/LUX-YU/lux-engine/e8ae7eb45d44318dc30bd3eb615bacb4d3f0a4ad/.internal/evidence/script/script-v4/script-v4-raw.zip)；归档提交 `e8ae7eb45d44318dc30bd3eb615bacb4d3f0a4ad`，SHA-256 `0695676277c4113422ddf2f849e21d76c8dc4fa5d2b4ad45f0833fd0243e624e`。
归档约 4.90 MiB，562 项原始文件逐项验证。
归档和本文提交不重标为新测试身份。

仅 Lua 5.5.1 + `lux-lua55-v3-r2`、Native ABI6、Windows x64、MSVC 19.44.35228.0、RelWithDebInfo `/MD`。
固定安装依赖 lux-cxx `3100f54d0743c5ed94a4ccf5943df04e933de255`、toolset
`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`，204 个安装文件哈希复核。
正式 VM `api_check=OFF/diagnostics=OFF`；独立 API-check VM 开启 LUA_USE_APICHECK 和 LUAI_ASSERT。
未改变上游 VM patch、Native ABI、调度、step/frontier、预算、Event cutoff/single-flight 或 thread 可观察身份。

## 1. 阶段状态

| 阶段 | 状态 | 实际提交 / 工作 |
|---|---|---|
| W0 | DONE | `00000503`：内层 callback 自己预留额度；API-check 修前断言、修后深度/OOM恢复。 |
| W1 | DONE | `40b83141`：固定 class/阶段观察；一次当前软件 Hotspots。GC state/debt/cycle未采集。 |
| W2 | DONE | `72153e94`：384B档、同档空页优先、保留块头、独立全局最旧空页淘汰；同一16MiB硬预算。 |
| W3 | DONE_KEEP_G0 | `4e7fa518`：固定W2比较INC原值、pause150、GEN；未见联合收益。32MiB单列，未改默认。 |
| W4 | DONE | `412bcca2`：必需状态/可选观察分开；24B票据；Native发布关系失效并保留物理vector容量。 |
| W5 | DONE | `66654416`：N×R域保证的小frame放置；64271a83补真实生成与destroy重入/start失败清理。 |
| W6 | DONE_WITH_LIMITATIONS | `f7d2815b`：clean clone全量、SDK/增量/迁址、原五腿和两后端短组、独立资源诊断。 |

公共 LuaAllocation 头已同步 Debug/RelWithDebInfo/Android 三个既有安装头前缀及当前 v4 SDK，逐一同哈希。
Android 仅同步头，没有构建验证。NativeFrameStorage 留在 private pinclude，两个产品的安装头均未泄露它。
真实 C++/Native/FlowForge 使用新的 storage/票据；现有安装消费者重新生成/编译，未引入旧模式兼容层。

## 2. 关键结论

**内层栈缺口真实存在，已经修正。** 修前 `2de21a10` 的 API-check VM 在深度16触发
`L->top.p <= L->ci->top.p`，退出3221226505。修后 trampoline 在自己的 CallInfo 内按 Operation
额度 checkstack，简单操作8、深度1/16/32计划额度12/72/136；失败返回VM_FAILURE，外层拥有型对象正常清理。
未改 MINSTACK 或支持深度。最终源码又运行5条 API-check命令，深层/OOM/恢复、正式生成provider、页与协程合同通过。
MSVC 的深层 expected<T> 构造探测曾触发C1054；最终最大深度用同一 typed readPlan 与 consumePlain 分别验证，
普通公开 Codec::read 旧用例保留。这里不声称最大深度的公开 expected 包装器也已经编译通过。

**页供给主要发生在新调用/登记区间，完整空页被硬预算淘汰后又供给。** W1保留v3分配算法、加入有界观察的
`40b83141` 记录3000周期×4点，共12000条、丢弃0；最终v4相同。2000个计时周期的供给/释放均落在
phase0→1（创建thread、参数初始化、调用并登记等待），phase1→2完成来源和phase2→3恢复没有这些增量。
这是有限边界记录，不是逐条Lua API计时。GC内部state/debt/cycle仍为null。
例如cycle1010，v3活动页48,955,392→23,396,352B、释放304页；v4为45,088,768→22,151,168B、释放243页。
该区间同时有新分配和live下降；结合GC调用栈支持“分配触发回收波动”，不能据此给出准确GC子阶段。
所有ROI页释放的原因都是idle_limit，trim/shutdown/direct fallback均为0；1000次预热没有消除超过空页预算的波形。

v3热档512B承接每次360B LX，736B承接720B stack。最终384B档中360B对象的含header stride为392B，
每64KiB页166槽；旧512档stride520B、125槽。LX本体仍360B、stack仍720B，没有合并或复用可观察thread对象。
同档空页保留块头和carved/free链；跨档只重置空页。16MiB包含全部完整空页，无隐藏reserve。
元数据Page由80B增至96B；这笔成本计入统计。小对象取整下降不等于RSS同幅下降。

**GC保持INC原值。** 实际上游参数是pause250、step_mul200、step_size9600、minor20、major_minor50、minor_major68。
在固定W2 `4e7fa518` 上各跑一次Event/原record计时与独立诊断：

| 筛选配置 | Event ns/完整周期 | record ns/完整调用 | Event ROI页供给/释放 |
|---|---:|---:|---:|
| G0 INC原值 | 831.019 | 1345.745 | 94591 / 94716 |
| G1 INC pause150 | 951.170 | 1353.075 | 14288 / 14213 |
| G2 GEN原值 | 975.203 | 1329.083 | 95840 / 95840 |

没有联合收益，未采纳G1/G2。这是有限筛选，不是三对正式结论。唯一额外32MiB试验仍用G0：
Event单次699.884ns、ROI供给5060/释放5001；其观察到的active+idle峰值52,953,088B，
高于默认45,088,768B。进程峰值受波动影响并未单调增加，不能拿该单次比较代替默认三对，生产仍16MiB。

**C++删掉了凭证冗余和可选统计写入。** 当前COFF显示CppSequence编译器frame请求仍384B；
FrameHeader从40B→24B，含对齐最坏overhead47→31B。在原512B上限、10000容量下，
stride560→544B，arena5,600,000→5,440,000B；当前metadata240,280B（每slot24B加固定元数据）。
body、header、slot metadata是不同部分。active/free/完整generation/capacity是必需状态，步骤/峰值/live/occupied为可选观察。
原所有权和64位代次检查保留。128次10000个start/resume诊断在预热后EXE-local operator new=0；
实际DLL机器码 allocateFrame→BoundedClassStorage::acquire 和 release链无下层new/malloc入口，Native acquire/release→link/unlink同样如此。
这是frame路径证据，未声称整个进程所有DLL堆调用为零。

**Native退休保留物理绑定buffer，先撤掉全部正式关系。** context、module/state与abilities/local_abilities/events
的逻辑项失效，capacity保留；32轮真实物化/退休验证状态重新初始化、绑定buffer容量恒定和最终零活跃。
prepared发布、失败回滚和宿主清理旧合同保留。retained_binding_bytes统计实际capacity，不能把保留buffer称为完全释放。

**Native小frame紧凑放置真实落地，但不节省最坏reserved。** 同一真实生成FlowForge模块16B/528B方法，
原N=8、R=528，payload reserved始终4224B、metadata784B。8个小frame occupied128B、active-region528B；
8个大frame occupied/active-region均4224B。小→大→小各8个均成功，第9个失败，provider次数/字段结果和最终释放准确。
随机混合、隔离/共享域、超对齐、错owner/旧generation、布局复用、destroy回调重入和start失败均验证。
证明：k<N个frame最多占k个非空region，因此总有空region可装任意合法大frame，不借别的population配额。

代价也保留：原FlowForge Event自身frame/envelope都是16B，N=10000，payload仍160000B，没有packing收益。
其报告metadata240312→1200272B；后者新计入现有continuation内嵌Lease的320000B，
扣除这项新口径后class/region等表仍净增639960B。不能把959960B全部叫新增独立heap，也不能隐去真实新增元数据。
这些容量在create时计入原预算，未提高预算；对紧预算配置可能更早明确拒绝。

**本轮采样支持有限归因。** W1软件Hotspots完整进程CPU20.016s、elapsed21.607s，包含准备/预热/检查/关闭。
高自耗时包括luaH_Hgetshortstr0.647s、traversestrongtable0.642s、sweeplist0.581s、luaV_execute0.580s、
stack_init0.554s、allocator release0.533s、ScriptExecution::resumeOne0.498s、allocator acquire0.438s。
Event脚本的`lux.Event.Benchmark.event()`包含Lua字节码_ENV/字段链；C++ continuation root/registry访问是另一条链；
本场景payload为scalar，不能把这些table热点全归给record shape/key/value转换。
这次采样在W2之前，不用它声称最终v4 cache miss/分支误预测已定位，也不重复引用旧26.3% longjmp比例。

## 3. 五条原业务与当前 C++/FlowForge

正式计时双方可选VM/allocator观察关闭；v4可选storage观察关闭，v3原实现的常驻storage统计保留。
单线程owner、affinity mask16、原worker/预算，AB/BA/AB各三对独立进程。
Event/NextStep/scalar为10000实例、1000预热、2000计时周期；record原ValuePose嵌套双向为1000预热+1000000调用；
Physics原混合业务300预热+2000帧。24份逐帧CSV工作量核对，加6份record完整provider/result/cleanup oracle。
Event完整30M周期的独立业务校验checksum930010000，正式ROI20M；所有正式组观测到的累计调用错误均0、最终backlog0。
以下成本是各侧独立中位数，百分比是三对比值的中位数，二者不应直接相除代替配对统计。

| 腿 | v3成本中位 | v4成本中位 | 三对变化% | 配对中位% |
|---|---:|---:|---|---:|
| event | 710.822 ns/完整调用或周期 | 709.613 | -2.667, -0.170, -0.368 | -0.368 |
| nextstep | 755.270 ns/完整调用或周期 | 731.464 | -0.159, -5.758, -0.712 | -0.712 |
| scalar | 141.300 ns/完整调用或周期 | 144.894 | +4.314, +3.957, +0.545 | +3.957 |
| record | 1264.532 ns/完整调用或周期 | 1313.828 | +3.898, +1.552, -0.510 | +1.552 |
| physics | 2837011.000 ns/整帧 | 2875797.250 | -2.024, +8.070, +1.367 | +1.367 |

负数为更快。Event/NextStep改变量很小；scalar三对均慢，record两对慢、Physics有−2.02%至+8.07%波动。
当前联合实现不能宣称全面CPU提速。W0多了真实必要的栈额度验证，但没有单因素对照，不能把scalar全部增量认定为必要安全成本。
没有删检查或减少业务来补数字，也未追加全面调优。

原五腿每对的**完整计时区间**：

| 腿/对 | 实际操作数 | v3总秒 | v4总秒 |
|---|---:|---:|---:|
| event/1 | 20,000,000 | 14.211443 | 13.832444 |
| event/2 | 20,000,000 | 14.216444 | 14.192252 |
| event/3 | 20,000,000 | 14.562153 | 14.508586 |
| nextstep/1 | 20,000,000 | 14.652599 | 14.629278 |
| nextstep/2 | 20,000,000 | 15.212487 | 14.336587 |
| nextstep/3 | 20,000,000 | 15.105403 | 14.997807 |
| scalar/1 | 20,000,000 | 2.825997 | 2.947921 |
| scalar/2 | 20,000,000 | 2.775968 | 2.885821 |
| scalar/3 | 20,000,000 | 2.882162 | 2.897870 |
| record/1 | 1,000,000 | 1.264532 | 1.313828 |
| record/2 | 1,000,000 | 1.301176 | 1.321367 |
| record/3 | 1,000,000 | 1.258242 | 1.251828 |
| physics/1 | 2,000 | 5.770107 | 5.653301 |
| physics/2 | 2,000 | 5.364170 | 5.797058 |
| physics/3 | 2,000 | 5.674022 | 5.751595 |

C++与FlowForge每backend三个有界配对批次，固定10000实例、300预热、1000计时帧，预算10000。
每批运行原有子场景；全部逐行业务计数一致。v3 FlowForge EXE不在原49文件镜像内，
使用覆盖构建槽前保存的e06208de内嵌身份EXE及原v3 SDK DLL补充镜像，未混入v4 DLL，单列其完整身份。
下表按各自入口命名，`micro-flowforge-*`驱动仍通过Simulation/ScriptSystem，不能冒充直接provider裸调用。
C++ coroutine micro是独立backend start和resume+destroy，不能与完整Lua Event周期横比；Sequence是混合等待协议整帧，
包含NextStep→Event→simulation delay→typed Ability，未另造一个C++普通Ability裸调用成绩。

| 当前入口 | 单位 | v3中位 | v4中位 | 三对变化% |
|---|---|---:|---:|---|
| scene-cpp-update-heavy | ns/complete-call | 3.452 | 3.409 | +5.390, -30.054, -1.222 |
| micro-cpp-coroutine-start | ns/backend-start | 59.785 | 51.472 | -14.525, -14.522, -11.610 |
| micro-cpp-coroutine-resume | ns/backend-resume-and-destroy | 32.694 | 34.337 | +3.082, +2.604, +7.848 |
| scene-cpp-sequence | ns/simulation-frame | 2644912.600 | 2665053.600 | -3.018, +0.761, +0.170 |
| scene-flowforge-update-heavy | ns/complete-call | 6.772 | 6.738 | -3.836, -1.234, -0.026 |
| micro-flowforge-ability-query | ns/complete-call | 5.375 | 5.496 | -7.540, +6.174, +2.251 |
| scene-flowforge-sequence | ns/simulation-frame | 2318577.200 | 2340908.100 | +1.594, -0.289, +0.963 |
| scene-flowforge-event | ns/complete-call | 312.987 | 313.423 | +0.139, -2.313, +0.598 |

C++ start减少约8.31ns（独立中位），局部resume/destroy增加约1.64ns；完整Sequence没有相应幅度的收益。
所有短组原始总时间、实际次数、p50/p95/p99批次尾部、错误可观测范围与backlog都在result.json及原CSV。
backend micro没有累计runtime错误计数，相关字段为null，依靠实际返回状态/checksum/回收断言；不把缺失字段变0。
单次操作尾延迟、逐次resume墙钟延迟没有测量，仍为null；已有步号/预算/关闭oracle保持，不能用批次尾延迟替代。

## 4. 内存与资源

Event独立诊断仍为同样完整工作，以下供给/释放取2000计时周期差值。W1 class/phase观察与v3原镜像aggregate计数一致，
class级新增观察的源码身份仍标40b83141，不伪写为旧镜像本来含有该诊断。

| 范围 | v3 | v4默认16MiB | W2独立32MiB试验 |
|---|---:|---:|---:|
| ROI logical alloc/free | 40,000,000 / 40,020,000 | 40,000,000 / 40,020,000 | 40,000,000 / 40,020,000 |
| ROI requested bytes LX+stack | 21,600,000,000 | 21,600,000,000 | 21,600,000,000 |
| ROI page/heap supply/release | 128,270 / 128,441 | 94,591 / 94,716 | 5,060 / 5,001 |
| ROI same/cross class reuse | 125,463 / 116,879 | 235,256 / 842 | 325,589 / 40 |
| ROI slot header writes | 37,973,573 | 10,864,700 | 579,817 |
| VM requested live peak B | 47,183,630 | 47,183,639 | 47,183,612 |
| 四点观察active+idle峰值 B | 48,955,392 | 45,088,768 | 52,953,088 |
| 四点观察pinned free峰值 B | 1,168,768 | 1,275,904 | 1,275,904 |
| 结束active / idle B | 36,175,872 / 1,572,864 | 33,619,968 / 3,276,800 | 33,619,968 / 17,956,864 |
| 结束rounding / metadata B | 5,856,370 / 1,232,192 | 3,296,233 / 1,206,848 | 3,296,260 / 1,206,848 |
| 结束direct backing B | 237,528 | 237,528 | 237,528 |
| 进程peak WS / private B | 110,133,248 / 107,876,352 | 106,844,160 / 104,611,840 | 106,512,384 / 103,313,408 |

页是下层heap块，heap调用不是OS syscall。Requested/live/active/idle/pinned/rounding/metadata口径分离，
不同时间的峰值不可相加；direct表中为结束快照，不是未采集的direct峰值。VM内部瞬时requested peak高于四点观察live peak属正常采样差异。
当前12000条固定phase记录无丢弃。结束前requested_live不是泄漏：Lua GC仍可持有已完成thread，shutdown/close与allocator释放合同另由真实测试验证。

record原嵌套业务的一次独立诊断：v3/v4各8,008,772次ROI VM逻辑分配，未减少返回table数量或复用table身份。
v4结束active589824B、idle0、pinned489664B、rounding20855B，累计页供给9/释放0；旧镜像没有这些record细项，保留null。
两侧进程peak WS为8,634,368/8,749,056B，private为1,900,544/1,949,696B；一次观察不作内存等价结论。
Native Event-idle诊断10000个真实挂起frame后active10000、payload160000B，关闭acquires/releases各10000、active/live/occupied归0；
绑定buffer retained880000B持续至backend销毁。可选统计关闭的正式CSV原0字段依observation标记解释为未采集，不作为0成本。

## 5. 正确性、安装与失败候选

最终资格绑定f7d2815b，先ValidateTrackedSnapshot，再从独立clean clone配置；Toolchain/Developer均执行
`cmake --build <build> --target all -j 4 -- -k 0`，第二轮均`ninja: no work to do`。构建、测试、采样、计时串行。

- Toolchain CTest **110/110**、Developer **126/126**；原生命周期、权限、取消、Event/pin/frontier/预算、Scene及真实后端断言保留。
- 匹配Lua55标准VM **4/4**：smoke、上游Windows适配测试、leaf、CallInfo/close/记账；独立API-check最终5条命令全部通过。
- 当前SDK **15/15**消费者通过；值生成增量 **13/13**按预期通过（4项为必须拒绝的输入，exit1才正确），恢复后无工作。
- 两条迁址链重新生成/编译/执行通过。受控源码、build、原SDK/VM路径暂不可用，最后恢复；编译输入未使用引擎私有头或旧generated。
  安装值纵向链仍有legal=2、rejected=2、begin/end=1、provider ctor/dtor=1、lease/release=1、backlog=0；普通输入错误之后合法恢复。
- 15消费者名单、每个源码身份/EXE哈希、SDK依赖和逐命令退出码见result.json及归档；没有重跑旧VM矩阵。

子提交失败/未采用项全部保留：W0深层fixture的MSVC C1054尝试，W3配置初始化顺序编译修正，
FlowForge fixture prepared类型及Graph pins构造时序修正，首次all的三个格式门禁失败。
最终资格与全部正式性能过程无退出失败/无效业务组；驱动拼写/缺少Python导入的失败没有混入有效组。
G1/G2未采用，32MiB不进默认。没有把失败阶段的产物重标为最终clean候选。

## 6. 停止与保留限制

W0/W1/W2/W4/W5已实际编码；W3完成有限选择并保持默认；W6实测交付完成。
性能维度保留scalar、C++局部resume新增成本和其他波动，未解释份额未关闭，也没有认定架构收益足以抵消。
Windows结果不推导Linux/Android/其他编译器、任何游戏工作负载或性能等价。
原main工作区7项未知修改的内容哈希、状态与HEAD保持；依赖未升级、旧镜像未覆盖。
提交/普通推送沿用授权；不合并main、不发tag、不强推。到此停止等待v4统一审阅，
不进入table IC、thread-stack合并、固定bytecode或新语言/资产功能。
