# SR-6 联合候选与资格

入口为72b46285，生产参照E=f64cadde；依赖lux-cxx 3100f54d、toolset 99c3d048保持。
本轮明确批准SR-6，保留已量化成本，不宣称性能等价；不扩类型、合并main或打发布tag。
实际工作位于既有s5/source分支，新资格目录为build/RelWithDebInfo/s6；main七项未知文件与E产物已固定哈希。

## 开工职责映射

| 协调/唯一owner | 实际状态与正式入口 | 失效与清理 | 对应真实验证 |
|---|---|---|---|
| ScriptSystem | prepare、processLifecycle、executeStablePoint、shutdown；跨owner顺序 | 先撤权限，等待保护；统一故障记录 | lifecycle、hook execution、move assignment |
| ScriptInstances | mounts_、invocation_states_、methods_、changes_；装配票据、调用票据、query/collect | 完整代次及原窗口；领取一次回收资格，资源结束维护reclaimed | lifecycle/reclaimed、admission、七点重入 |
| ScriptBindings | endpoint目录、配置/绑定范围、handlers、token、pending_unlinks_ | 发布回滚、逻辑失效与延迟unlink；busy保留token | bindings rollback、跨批次顺序 |
| ScriptPreparer | 资产resolver、prepared capability目录及准备票据 | 提交前回滚，提交后归Instances；目录覆盖实例寿命 | prepare/BeginPlay失败与资源计数 |
| ScriptExecution | execution_instances_、active_hooks_、Continuation/Awaitable、唯一最终结果及ResumeRing | 来源双向取消、copy pin、完整身份、取消/退休/销毁 | continuation、event、pin、frontier、预算 |
| ScriptEventWaits | waiter来源、实例索引、路由/登记序号、claimed_ | occurrence claim→callbacks→complete；取消和unlink | sealed batch、嵌套claim、跨reset结果 |
| ScriptTimers | NextStep来源、模拟时间heap_及instances_索引 | 显式来源取消及物理槽位复用；不改deadline/稳定点 | Timer取消、同deadline顺序、容量 |
| ScriptCompletionIngress | 外部transport、准入lease、队列/frontier和关闭 | 不拥有最终结果；关闭/迟到/旧代次拒绝 | ingress并发、frontier、背压 |
| Lua值模块/backend | 生成codec、typed slots、protected primitives、prepared projection | 初始资格；可重入转换后原资格复验；逆序清理 | 真实生成/runtime；本轮安装provider纵向链 |

本表按本轮源码复核，后续补入C01—C13最终断言、安装链与实际结果。既有SR-1旧阶段缺口不直接当作现存失败。

## 最终正式路径与C01—C13

F=`7b5e1dd4f824e0a2746b1dbb0ec2d0dcc57fe490`。`engine/`、`modules/`与E逐文件Git比对无差异，包含模板。
本轮生产运行时无补丁；新增/修改的是安装consumer、增量执行检查、scale统计及独立协议诊断。
audit/formal-paths.json记录七个私有owner头身份和各契约的实际测试文件/函数；最终执行结果另列。

| 契约 | 实际正式入口/行为 | 现有断言与本轮补充 |
|---|---|---|
| C01 单图/排他 | Simulation编译执行区域、caller Hook authority与原TaskGraph | hook_regions的1..9顺序、两个Simulation barrier、失败producer/command；本轮regions诊断在整个worker与owner窗口加原子互斥检测，避免仅看线程ID |
| C02 通用step协议 | Backend step/resume/destroy→Execution | testSyncAndContinuation、testAsyncAbilityInvocation：eager/late结果42、duplicate拒绝、失败91、拒绝81；真实Lua/CppStatic/Native/FlowForge测试仍运行 |
| C03 step/frontier/预算 | executeStablePoint按真实step去重；ResumeBatch每pop先扣预算；每次pop后同frontier接入 | testResumeBudget、sealed真实step、script_ingress_frontier_test；本轮17 READY结果、预算3，在step1..6完成，不加恢复点 |
| C04 occurrence顺序 | System claim→普通callback→complete；来源组件与Execution分权 | testBroadcastSemantics值42/42；testTargetedAndRetirement销毁后不恢复；NestedDispatch及跨批次trace |
| C05 cutoff/self | EventWaits登记序号、路由、完整实例资格 | testRegistrationCutoff、testTargetedScopeRejection；testSealedBatchAndOwnedResultAcrossSteps一次batch中两个occurrence，callback新wait只吃第二项 |
| C06 唯一结果owner | Execution拥有最终结果，ResumeRing只通知 | Channel reset并写999后仍在真实step1/2读10/20；本轮reset后17次恢复均读73 |
| C07 重入与稳定地址 | 调用票据、ResultWritePin、claim/cleanup保护 | copy中退休pin/deferred闭环、另record删除、nested admission；七点CREATE/PREPARE/INVOKE/continuation destroy/releaseMethod/backend destroy/lease release；同步退休错误仍保留 |
| C08 身份与权限 | 完整实例代次、retirement epoch、prepared frame与authority类别 | 旧completion INVALID_ID、incarnation重建、prepared provenance、Lua旧closure；四项retire/stop × scalar/zero、合法recovery、standalone及Begin/End |
| C09 内外完成 | Ingress只运输；Execution完成/关联/最终结果 | frontier测试预留未发布不得超车、窗口外消息保留、重复/满队列/关闭/旧代次；异步provider真实线程完成结果42 |
| C10 构造/退休/移动 | Preparer提交前回滚；Instances领取一次清理资格 | 初始批次首Begin时create=3、Begin失败不补End；移动活动/挂起/空/自移动及busy；本轮安装provider ctor/dtor=1、asset lease/release=1 |
| C11 prepared热路 | 初始provider绑定与已生成类型操作独立；运行中使用prepared | 真实Lua prepared storage/贡献生成；新增安装SDK生成→Lua输入→provider→独立字段结果；没有运行期字段反射发现 |
| C12 装配/结构/失败 | loader持描述与World解析；runtime接owned resolved Entity配置 | OwnedRuntimeInput、ResolvedBatchProtocol、MixedReassociationRollback、InputExpiryAndReuse；ASSET_NOT_RESIDENT为FAULTED/REJECTED/reclaimed，query不消费、部分collect保留剩余 |
| C13 容量/回收 | Timer显式取消、来源双向闭环、固定backing与配额 | 32次取消/重建、同deadline Ring容量1下一真实step重试；8/8192配置只重建一个、固定16warmup+128；停产后合法完成drain与shutdown取消分开 |

复核没有发现需要删除的存活过渡入口。生产中LuaRecordMarshaller、runtime WorldObjectResolver/WorldObjectId/持久化描述均无匹配。
`ScriptRuntimeAssembly`在L3加载侧仍有实际Scene/authoring消费者；不能把它误当fallback删除。
sol2对象绑定、测试专用authority工具、ProtectedRecord诊断、普通meta消费者及内部配置/实例/来源ID保留其不同职责。
七组件不共享完整System State；private记录不通过可写容器公开。Instances的collect写调用方buffer，非返回内部可写状态。
LuaSlots/字段规则未进入Execution；新的consumer仅公开SDK头，无Engine测试helper或pinclude。

## 作者可用范围与安装示例

首批仍为bool/i32/u32/float/double、有限i32/u32 enum、opt-in有限嵌套record及显式自定义表示。
同步typed Ability支持既有输入/输出方向；输入borrowed_step，结果owned_value及当前trivially-copyable约束不变。
strict raw table不接受缺失/额外/错误字段；不以metamethod补字段。push-only自定义表示不回退另一种reader。
typed slots按实际参数构造并逆序清理，无须默认构造全部T；const/省略字段按既有构造策略或显式factory处理。
最多64字段、深度32、展开frame最多64KiB；不扩对象图、容器/字符串通用转换或非平凡跨挂起结果。

可运行示例就是 `cmake/installed-consumers/script-lua-values`：自有Values.hpp/Ability.hpp/Rules.hpp经过
安装的 `lux_script_lua_values` 与 `lux_script_abilities` 生成；CMake显式find/link simulation_script、Lua backend和Simulation composition。
运行 `cmake -S <consumer> -B <fresh-build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=<SDK和固定依赖>`，
再运行 `cmake --build <fresh-build> --target all -j 4 -- -k 0` 和生成的consumer EXE；完整平台参数以relocation日志为准。
公开路径是SimulationBuilder发布provider和Hook→Simulation::create→ScriptSystem::create/prepare→bindHookCallbacks→execute→shutdown。
消费者声明唯一稳定Hook，采用两worker执行器；没有伪造Hook token或增加第二个Scene调度图。

运行的Lua正例：`local x=lux.Values.echo({key=7,weight=2.5}); assert(x.key==24 and x.weight==9)`。
provider以`id*3+3`、`weight*2+4`返回，预期由手写Lua和C++保存的输入分别检查，不由codec反向生成期望。
两个独立Hook阶段分别捕获缺weight和错误weight，provider累计数都不增加；之后合法输入11/1.25返回36/6.5。
Begin/End各调用一次provider，普通成功两次，总数4；同一Lua对象的phase断言还验证生命周期不重建。
错误用`local ok,err=pcall(lux.Values.echo,{key=7})`捕获并检查string；输入失败不调用provider，结果写失败不能撤销已执行业务。
captured错误的顶层failures允许为0，不能拿它替代资格或业务计数。C++ SDK宿主/投影布局版本变化要求重编，
本报告不以C ABI导出名未变承诺任意旧DLL二进制兼容。

## 验证、成本及交付结果

本轮在2026-09-08完成串行验证；资格源码F为上列7b5e1dd4，后续报告/证据提交不改变该产物身份。

| 状态 | 本轮结论 | 原始记录（归档内相对路径） |
|---|---|---|
| IMPLEMENTATION | PASS，安装纵向缺口已补齐；正式运行时/模板未修改 | audit/formal-paths.json；evidence-stage/final-identity.json |
| CORRECTNESS | PASS于本轮矩阵；Toolchain全量108/108、Developer全量123/123、Lua54受影响子集110/110 | q/qualification.json及t/d/l的CTest日志 |
| CODEGEN_INSTALL | PASS，15个消费者、13项增量检查、新位置生成/编译/纵向执行；第二次构建均no-op | consumers-final、incremental-final、relocation |
| PERFORMANCE | 已完成同量记录，保留旧债务；按下表单项分类，不授予统一性能PASS | performance、record-costs、probes、scale、extra-costs、summary/costs.json |
| EXTRA_PLATFORMS | NOT_TESTED：Linux/Android/额外Debug或Release/完整Editor、Lua54全profile与全部消费者 | 本轮只有Windows RelWithDebInfo；Lua54仅上述子集 |

没有新增CTest注册；新增的是已安装消费者内的断言和独立诊断。原六项Scene Lua测试使用匹配的实际资产通过，
本轮既不重新修这些预期，也不继续记作失败。七点重入、同步错误、Bindings回滚、身份复用、Event/pin/frontier/预算、
装配规模、四项真实Lua资格负例和合法recovery/standalone/lifecycle都包含在实际受影响验证中。

完整构建均为`cmake --build <build> --target all -j 4 -- -k 0`，固定clean tracked源码，构建、测试、计时串行。
未改modules公共头，无新的三前缀同步内容；私有owner头未进入SDK，实际direct runtime链接无World/Scene/Process，
description leaf未携带Scene runtime或Process。资产、VM、生成器、EXE/DLL、SDK和实际链接身份见identity及final-identity。

### 安装纵向链和relocation

原codec/contribution断言仍先执行。真实provider按阶段累计为2→2→2→3，两个错误阶段各自增量0，随后恢复成功；
Begin/End各1次，普通合法调用2次，总provider调用4次。provider构造/析构1/1，资产lease/release1/1，
第二次shutdown无重复释放，active/continuation/awaitable/waiter/queue与prepared slots均归零。

relocation在新`s6/relocation/sdk/{d,t}`安装位置，从consumer自有类型重新生成；原候选源码、构建和SDK目录实际临时移开，
禁用包注册表/旧搜索前缀，所有解析的Engine包目录及编译/链接/命令路径检查通过。原目录在finally中恢复。
仅LuaJIT的这个纵向消费者做了relocation，不扩大为Lua54全部SDK迁移资格。

### 同量成本E→F

每场景五对独立进程，偶数对E/F、奇数对F/E；Windows 24逻辑CPU，MSVC19.44，RWDI /O2 /Ob1，
固定工作量/seed/容量/预算/warmup/VM默认GC，不锁频、不绑定CPU亲和性。没有并发构建或测试，保留所有配对，未丢弃慢样本。
完整参数与顺序见evidence-stage/cost-plan.json及各runs.json，正式计时与trace/分配诊断分开。

下表E/F分别为各自批量总时间中位数；差值及百分比按同一对计算后取中位，不能用两个边际中位数相减替代配对差。
单位成本摊销完整批量，不是单独的resume、handler或权限检查成本。IMPROVED只是这五对的观测，生产源码相同，不能当作本轮优化收益。
配对异号标NO_MEASURABLE_GAIN仅表示没有一致方向，不构成统计等价证明；五对同正标未解释回退，不用源码相同或旧债务豁免。

| 场景 | 实际操作分母/进程 | E / F总时间 ms | 配对差中位 ms | 配对差 ns/操作 | 五对变化（0..4） | 分类 |
|---|---|---|---|---|---|---|
| cpp-update-10k | 50,000,000 actual_new_calls | 225.828 / 228.712 | 2.884 | 0.058 | -3.51% / +5.85% / +1.15% / +1.28% / +1.59% | NO_MEASURABLE_GAIN（配对异号） |
| lua-update-10k | 50,000,000 actual_new_calls | 1,289.166 / 1,310.286 | 21.120 | 0.422 | +11.17% / -8.11% / +1.64% / +9.02% / -3.14% | NO_MEASURABLE_GAIN（配对异号） |
| lua-ability-10k | 30,000,000 provider_calls | 2,565.044 / 2,529.133 | -3.759 | -0.125 | -5.33% / -0.15% / +1.24% / +6.90% / -3.94% | NO_MEASURABLE_GAIN（配对异号） |
| lua-coroutine-10k | 10,000,000 actual_new_calls | 10,752.689 / 10,606.311 | -16.844 | -1.684 | -2.70% / -0.00% / +1.00% / -2.37% / -0.16% | NO_MEASURABLE_GAIN（配对异号） |
| lua-event-10k | 50,000,000 actual_new_calls | 54,315.408 / 54,507.824 | 304.746 | 6.095 | -11.45% / +3.80% / +5.13% / -3.67% / +0.56% | NO_MEASURABLE_GAIN（配对异号） |
| flow-update-10k | 100,000,000 provider_calls | 461.825 / 429.986 | -11.475 | -0.115 | -12.19% / +9.53% / -2.48% / -2.10% / -11.39% | NO_MEASURABLE_GAIN（配对异号） |
| flow-event-10k | 10,000,000 provider_calls | 5,864.821 / 5,884.520 | 88.794 | 8.879 | -0.69% / +2.56% / -2.17% / +2.15% / +1.55% | NO_MEASURABLE_GAIN（配对异号） |
| event-fanout-10k | 10,000 resumes | 2.035 / 2.032 | 0.049 | 4.890 | -12.98% / -0.30% / +2.47% / +9.96% / +7.99% | NO_MEASURABLE_GAIN（配对异号） |

除fan-out外，逐frame工作量、checksum、起停计数、continuation/awaitable/waiter/queue均逐行对拍。
Lua scalar为30M provider，普通Update按实际new-call而非handler候选计；Flow Update每次new-call含两次provider。
Lua coroutine和Flow Event各50M Hook候选、10M实际新调用/恢复，持续backlog=8000；不得把50M当作provider分母。
Lua Event沿原场景使用实际10000恢复预算，50M实际新调用/恢复、每frame一个Event，backlog=0；CLI的2000不是该场景实际预算。
fan-out一次occurrence、10000完成/恢复，5个原预算2000的真实drain step。INTEGRITY是计时外可观测范围，
旧errors/failures缺列仍为null；不是零，也不承诺捕获所有已由Lua pcall处理的错误。

| 值场景 | 实际输出/provider数 | E / F总时间 ms | 配对差 ns/操作 | 五对变化 | 分类 |
|---|---|---|---|---|---|
| record | 1,000,000 | 251.337 / 248.109 | -3.349 | -1.35% / -3.24% / +0.30% / -1.29% / -1.84% | NO_MEASURABLE_GAIN（配对异号） |
| typed | 100,000 | 139.068 / 136.727 | -9.325 | -4.35% / +1.75% / -0.67% / +2.51% / -1.80% | NO_MEASURABLE_GAIN（配对异号） |

typed指当前首批Pose真实双向provider，不制造旧版本不存在功能的历史比值。两字段record和typed均保留errors=0、backlog=0和完整业务数。
每个独立进程仅一个批量结果，不提供虚构的逐操作p95/p99。

| 冷期/退休批次 | 操作数 | E / F总时间 ms | 配对差 ns/操作 | 五对变化 | 分类 |
|---|---|---|---|---|---|
| 首次 create+prepare（8配置容量，1实例） | 1 | 0.143 / 0.145 | -900.000 | -0.61% / -0.63% / -4.83% / +3.39% / +4.48% | NO_MEASURABLE_GAIN（配对异号） |
| 迟到挂载（同批7实例） | 7 | 0.012 / 0.012 | 57.143 | +3.20% / +6.25% / -3.45% / -9.84% / +9.92% | NO_MEASURABLE_GAIN（配对异号） |
| 退休重建 | 128 | 0.106 / 0.108 | 16.406 | +1.03% / -11.06% / +3.12% / +1.99% / +3.73% | NO_MEASURABLE_GAIN（配对异号） |

冷期每进程create/destroy=152/152，prepare/release=456/456。保留6个生命周期与8个Event轨迹的实际CASE/事件计数，
所有插桩替换准确命中，非空轨迹E/F一致，wire golden 288字节一致。单次prepare/late没有逐操作尾部，128次重建尾部另见规模测试。

| 配置数（只重建一个） | 128次E / F总时间 ms | 配对差 ns/重建 | 五对变化 | 分类 |
|---|---|---|---|---|
| 8 | 0.097 / 0.095 | -17.969 | -2.38% / +0.76% / -35.81% / +68.64% / -30.59% | NO_MEASURABLE_GAIN（配对异号） |
| 8,192 | 0.099 / 0.098 | -13.281 | -1.52% / -6.15% / -2.03% / +0.51% / -1.69% | NO_MEASURABLE_GAIN（配对异号） |

两种规模固定16次warmup、128次计时重建；仅触及128个配置槽位、0个endpoint计数，已发布地址稳定，backing不增长。

| 实际集成场景/帧类别 | 样本数/进程 | E / F总时间 ms | 五对变化 | 分类 |
|---|---|---|---|---|
| scene-cpp-sequence | 200 | 194.464 / 196.394 | +3.87% / -1.66% / +0.57% / +0.47% / -1.58% | NO_MEASURABLE_GAIN（配对异号） |
| scene-cpp-sequence-drain | 10 | 6.316 / 6.078 | +3.00% / -3.40% / -0.83% / +2.04% / -6.64% | NO_MEASURABLE_GAIN（配对异号） |
| region-graph-prepare | 1 | 0.117 / 0.106 | -7.88% / -2.94% / -3.06% / -11.87% / -13.84% | IMPROVED（本次观测） |
| scene-region-numeric | 256 | 100.369 / 99.535 | -2.67% / +0.45% / +2.03% / +0.60% / +1.17% | NO_MEASURABLE_GAIN（配对异号） |

sequence完整路径停止生产后，按原来源/时间/预算在10步内业务完成：最终calls/provider=150000、suspend/resume=450000、
checksum=6000000，执行资源与队列均0。该drain发生在shutdown前，不是取消伪装完成。
regions使用两个worker、65536元素、每frame两个真实CppStatic Script Hook；每帧工作量/checksum及图规模E/F一致。
另外独立完整窗口排他诊断记录worker=21、owner=82、violations=0，保留原双Simulation barrier及失败路径断言。

### 尾部、实际恢复延迟与内存

`summary/costs.json`按每run/场景列p50/p95/p99、总时间和maximum，nearest-rank，禁止平均不同run的分位数。
至少20个样本才提供p95，100个才提供p99；fan-out/短drain/prepare等不足样本字段为null。scale单独保留128次重建的p50/p95/p99。
创建、持续帧、恢复drain、图prepare各自分组，record/typed逐操作尾部未测。

独立协议诊断E/F均：step0完成17个结果并reset Channel，原预算3，在step1/1/1/2/2/2/3/3/3/4/4/4/5/5/5/6/6实际恢复，
每次结果73，17次destroy、errors/backlog=0；没有额外稳定点。该诊断无stale通知，stale pop仍由既有测试与源码扣预算位置核对，
不把这17项当作新的stale实测。它使用测试fixture，不混同只用公开头的安装consumer。

内存分层记录：scale分别给mount/prepared method/binding/feedback/Awaitable固定bytes；READY诊断给17个执行资源/queue和57544字节backing；
continuation与来源的独立字节未由接口暴露，保留计数/高水位而不虚构字节。具体backing数以scale/protocol日志为准。
typed内存诊断Item=16、slots=24、failure=258、input/result frame上界各3000字节；后两者是保守构造规划上界，不能合计成常驻栈或堆开销。
record单独10000输出VM诊断20000次分配、1120000 bytes、30000 protected C调用；typed 100000调用VM分配800000次。
EXE-local allocation关闭时的0均为未观测；打开后冷期prepare/late/remount计数5/33/535，scale重复重建诊断独立列出。
`extra-costs/runs.json`保留每进程peak working set与peak pagefile，覆盖整个启动/准备/计时/关闭，不等于具体owner存储，更不把arena当RSS。

| 固定backing（bytes，E/F一致） | 8配置 | 8192配置 |
|---|---|---|
| mount / prepared method | 3456 / 1344 | 3538944 / 1376256 |
| binding / feedback | 928 / 40 | 950272 / 40960 |
| Awaitable | 57480 | 57480 |
| 128次重建EXE-local诊断分配数 | 535 | 530 |

| 整进程peak working set（bytes，五次范围） | E | F |
|---|---|---|
| sequence | 42586112–42741760 | 42668032–42950656 |
| regions | 9867264–9969664 | 9953280–10018816 |

进程峰值包含装载器、DLL及分配器，F高出的部分没有独立归因；不能从这些范围证明owner泄漏或内存等价。
原始每对peak pagefile另列于runs.json。8配置计时配对曾出现−35.81%到+68.64%的波动，全部保留；
该约0.1ms的128次批量不能支持细小差值的稳定因果结论，也没有用重测替换这些样本。

### 保留的历史债务、无效尝试和限制

历史C0→E scalar +9.151%/+6.9321ns/provider、coroutine +8.099%/+78.8398ns/new-call；H→E scalar +25.648%/+16.7296ns/provider，
当前record相对特定protected手写诊断的约116.4941ns残余，均保留原f64资格/原始证据及其安全差别。
本轮没有重测H/C0，不把上述比值改标签为H/C0→F，也不跨轮相加相乘。SR3/4 Event及冷期/内存债务继续按原版本记录。
新增配对变化按本轮表格独立报告；没有为了追平删除安全、少做业务、改单侧优化选项或制造生产补丁。

两项无效开发尝试保留：首次consumer遗漏唯一稳定Hook，公开入口正确拒绝，修正fixture后通过；首次memory脚本将Select-String参数颠倒，
EXE已成功，读日志驱动失败，改为显式参数后在新目录通过。均非生产runtime回退，不混入有效计时，也未删除原日志。
未发现新的联合正确性缺陷。性能接受与否由最终审阅决定；本轮不以架构收益或安全检查存在证明全部残余合理。

所有原始记录及固定下载入口见[evidence/script/sr6/README.md](evidence/script/sr6/README.md)。
本轮完成SR-6联合候选，等待最终审阅；不合并main、不打tag、不冻结框架、不自动开始下一阶段。
