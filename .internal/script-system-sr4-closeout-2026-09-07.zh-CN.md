# SR-4 实施交付与限制（2026-09-07）

四个私有 owner 已建立，G03 Timer 显式取消已完成；最终 clean tracked 快照
`1750ce854967a382ee89accf3a3534a0628396cc` 的 Toolchain 107/107、Developer 119/119、14/14 安装消费者通过。
本报告交付 SR-4 结构迁移，**不认定性能等价或成本完全收口**。既有 Event 债务和两项小幅新增成本分别列在下文。
停止于 SR-4，等待独立审阅；未进入 SR-5/6、合并 main 或冻结框架。

## 身份与范围

本轮用户授权更新了此前禁止进入 SR-4 的停止条件，接受 Event 差距为待复核阶段债务，未接受其为必要安全成本。
开工本地与远端为 `ba6c7be44797e6e7efe4a272d0f208d247f7366c`。
E0 为 `8a6e6ef468f7eb60265678ab25116719b42a01a6`，生产代码保留 `99c1d095`；
H0 为 `8145598c18421d03da1dac21251200af3290e27d`。
lux-cxx 固定 `3100f54d0743c5ed94a4ccf5943df04e933de255`，toolset 固定
`99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`，使用已核验 `install/q2/c` 和 `install/q2/toolset`。

实现提交依次为：`89fb01bc` 修正已证明错误的 Scene Lua 逐步预期；`71e2f3aa` 迁移四个 owner；
`128754a6` 以唯一结果容量限制 Timer backing，撤销初版新增的组合容量拒绝；
`1750ce85` 增加 sealed batch/真实 step/Timer 背压测试并修正测量驱动的 Lua 资产定位。
其后提交仅涉及证据工具、记录和报告。最终生产 DLL 与 `128754a6` 相同，正式测试、安装和性能绑定 `1750ce85`。

工作树三项未知文件和 main 五项修改均保持原哈希，未提交。未 reset/clean/stash/rebase 或升级依赖。
修改的 `modules/*` 公共头已同步 Debug、RelWithDebInfo、Android 安装头；没有运行 Debug/Android 构建。
完整身份、安装头、生成器、DLL 与资产哈希见[增量证据索引](evidence/script/sr4/README.md)。

## 状态与操作迁移

完整线程、权限、借用期、用户代码边界、提交点、回滚、取消与销毁协议，以及旧断言到新入口的对应表，见
[所有权与协议映射](script-system-sr4-implementation-2026-09-07.zh-CN.md)。

| Owner | 实际责任 |
|---|---|
| ScriptExecution | 完整实例 ID 的执行索引、方法 single-flight、continuation、awaitable、唯一最终结果、ResultWritePin、ResumeRing、关联/完成/取消/失败与执行资源销毁 |
| ScriptEventWaits | waiter、路由、登记序号、claim reservation、每实例索引、取消 unlink 与统计；结果只通过有限关联交给 Execution |
| ScriptTimers | NextStep FIFO、deadline/sequence 索引堆、每实例来源链、登记/到期/显式取消/容量与统计；时间仍来自 SimulationClock |
| ScriptCompletionIngress | 原运输 ring、权限/lease、publication frontier、关闭准入、owner drain window；producer 不接触 Registry 或最终结果 |
| ScriptSystem | 初始/增量生命周期、故障与退休排序、每 occurrence claim→callbacks→complete、唯一 stable 顺序、每 pop 后同 frontier 接入与 shutdown |

Instances、Bindings、Preparer 的原权威保持。新组件都在原 simulation_script target 的 pinclude/src，未增加 DLL、
公共 System、通用 Runtime 或完整 State 共享。有限 FailurePort 只能上报一个实例故障；C ABI context 使用受限操作入口。
完整代次、原 invocation/resume/copy/claim/cleanup 保护及用户代码返回后重验保留。
Hook single-flight 仍是方法级，普通 Event 仍允许多飞；prepared Ability 继续直达 provider。

## G03 与六项 Scene Lua

Timer 现在有显式双向来源关联。discard、取消、退休和最终完成立即 unlink 自己的来源并安全返还物理槽，
NextStep 为 O(1)、模拟 delay 为 O(log n)，不扫描其他实例。旧通知仍按原预算作为 stale pop 处理。
因此 next_step_waits/simulation_delay_waits 在取消后立即下降是批准的可观察统计变化。
deadline、minimum_step、ceil(ns)、同期限登记顺序、背压与重试的真实 step 未改变。

同一最终新测试 EXE 使用 E0 安装 DLL 时，在取消后的物理来源计数断言失败；使用 E1 DLL 时通过。
新窄测试在 2 槽容量下做 32 次 discard、32 次退休重建，检查另一实例仍有效、旧代次完成无效、失败 starter 精确清理。
另有 NextStep/零 delay/正 delay、同期限两个实例、ResumeRing 容量 1 的背压保留与下一真实 step 重试，重复同 step 不重试。
显式单 sealed batch 含两个 occurrence，callback 新 waiter 消费第二个值；Channel reset 并写入 999 后，
按真实 step1/2 和预算1仍读取 owned 值10/20。

六项 Scene Lua 原失败不是被隐藏的运行时故障。实际宏指向 packaged `lua_portability_fixture.lxsa`，
E0/E1 SHA-256 均为 `d03c4a67cf102c2a404ca756a8cc3d11d4ecade49cc9418ddb88cfea147318ec`。
真实输入为 eager beginOperation→NextStep→simulationSeconds(2e-9)：step1 捕获 frontier 后发布，step2 接纳并首次恢复，
step3 NextStep 恢复后登记 delay；实测 ceil=2ns，elapsed3+2=deadline5，step4 应继续挂起，step5 完成1234。
旧 step4 的零 continuation 断言违反这一已有契约。修正加入逐 step 全状态/业务计数表及同 step 不推进检查，保留 EndPlay=-1，
未改资产、运行时、预算或恢复点。本轮重新跑 E0 六项仍在旧断言失败；E1 六项全部通过。
逐 step 内部 trace、实际编译宏/生成输入/资产身份、修前和修后日志均保留，不再仅标记“历史失败”。

## 验证结果

| 验证 | 最终结果与原始记录（归档内 sr4/ 路径） |
|---|---|
| 独立 clean clone、RelWithDebInfo 全量构建/安装 | candidate/qualification.json 中 source_sha=1750ce85：Toolchain 107/107，Developer 119/119；all -j4 -- -k0，串行；两 profile 第二轮均 no work |
| 真实后端与原安全回归 | lifecycle、continuation、Event/pin/frontier/预算、Bindings 回滚、七点清理重入/同步错误/身份复用、Scene、Lua/CppStatic/Native/FlowForge/Physics 全量 CTest 保留旧断言 |
| 安装消费者 | validation-final/consumers-driver.log：14/14；实际 runtime-input 链接无 World/Scene/Process；description leaf 独立，私有头未进入安装接口 |
| 轨迹与 wire | validation-final/probes：准确命中插桩、预期 CASE 与业务事件；6 生命周期+8 Event 轨迹相同；v1.bin 288字节，SHA256=8002ffd5678f822d79949b4d0a36d1687613216af4b26b876f967fa24cc76e52 |
| G03 修前/修后 | diagnostics/timer-source-proof-final/identity.json 与执行日志：E0 物理取消断言失败，E1通过；最终 DLL 身份固定 |
| Lua 定位与修复 | diagnostics/lua-before、lua-e0-six-current.log、lua-e0-six-LastTest.log、lua-fix-*、candidate/d/ 最终 CTest 逐步输出 |

先前 71e2、1287 的 qualification 和迭代日志单独保留，不能替代最终1750结果。
无效试验见 invalid-trials.json：未初始化 VS 的编译负例、重复单类型 layout 编译选项、漏传实际 Lua build-root 导致资产缺失的首次测量。
首次测量有25个有效子运行，但整矩阵无效；没有拼接到正式配对。修正驱动后两个完整矩阵重新执行。

## 同量性能：新增变化与历史债务分开

每个比较为六场景、五组独立进程配对；各72个进程（含独立分配诊断）、36组业务计数比较全部有效。
size=10000、seed=1592598566、warmup=60、worker=0、affinity=1；MSVC RelWithDebInfo /O2 /Ob1，无单侧 LTO。
C++/Flow Update 与 Flow Event 各3000计时帧；Lua各300帧。Flow Event预算2000、稳态积压8000；
Lua Event实际预算10000，两侧一致，未拿请求参数2000冒充实际值。fan-out 为一次交付加五次排空。
百分比是五个配对比值的中位数，批量时间是各侧五个总时间的中位数，二者不可用两侧中位数直接互算。

| 场景 | E0批量中位 ms | E1批量中位 ms | E0→E1配对中位%（范围） | H0→E1配对中位%（范围） |
|---|---:|---:|---:|---:|
| C++ Update |145.8120|142.0936|−0.84 [−14.25,+3.15]|−6.17 [−10.59,+9.83]|
| Flow Update |278.5843|277.3854|−0.60 [−2.70,+1.95]|−3.15 [−12.64,+2.86]|
| 持续 Flow Event |2964.9139|2909.5228|−2.27 [−3.13,−0.85]|+11.14 [+8.39,+11.83]|
| Lua Update |76.3082|77.7958|+1.52 [+0.51,+4.93]|−0.87 [−6.69,+6.52]|
| Lua Event |1979.4903|1940.3619|−1.88 [−3.93,−1.72]|+2.73 [+0.84,+6.02]|
| Event fan-out |2.2068|2.0672|−0.42 [−20.77,+28.75]|−7.81 [−33.90,+0.10]|

原始五对分布、两比较全部绝对时间、逐进程帧 p50/p95/max、实际操作归一化见 one-page.json/csv、
frame-distributions.json 和 cost-summary.json。fan-out 的交付与排空分开报告分位数；帧不当作独立进程样本。
有效工作：C++/Flow Update各3000万脚本调用，Flow另有6000万 provider调用；Flow Event有3000万Hook候选、
2400万single-flight跳过、600万新调用/恢复/provider业务完成；这里候选数由配置数×帧数计算，跳过数由候选减实际新调用推得，
不是新增的逐分支采样。Lua各300万调用，Event另有300万恢复；fan-out为10000恢复。
FlowEvent稳态queue/continuation/awaitable各8000，退出后为0且不增加业务工作；其余最终积压0。

E0→E1 未出现持续 Flow Event 新回退。H0→E1 旧债务仍为配对中位 +287.6392ms/3000帧，
即整帧摊销 +47.9399ns/实际resume、+0.0958797ms/本例帧；**不是 resume 函数的孤立成本，也未证明为必要安全成本**。
Lua Update 本轮五对均正，配对中位 +1.1735ms/300万调用，即 +0.3912ns/实际调用、+0.0039117ms/本例帧。
这是小幅新增、未单独归因的差异，不归入获准的旧 Event 债务，不宣称已消失、纯噪声或必要安全成本。

冷期 prepare/late/remount 各五组配对，业务 creates=destroys=152、prepares=releases=456：

| 冷期操作 | E0 / E1 中位 ns | 配对中位变化 | 配对范围 |
|---|---:|---:|---:|
| 首次 prepare |136000 / 144200|+7.61%，配对差中位+10000ns|+3.90%…+31.30%|
| 迟到挂载 |11600 / 11200|−5.17%|−74.11%…+4.72%|
| 单实例重建128次 |102700 / 102500|+0.88%，配对差中位+900ns|−4.88%…+2.24%|
| 8配置，仅重建一个128次 |93100 / 91600|−1.18%|−19.47%…+6.21%|
| 8192配置，仅重建一个128次 |96700 / 96600|−0.10%|−33.03%…+39.27%|

首次 prepare 五对均增加，是约10微秒的新增冷期成本；**未用布局差异冒充其已证明根因**。
两个规模均16次warmup、128次计时重建，槽位访问128、endpoint计数访问0、errors/backlog=0；
业务 creates=destroys=configs+144。短批有明显进程波动，表中负值不构成全局提速或等价证明。
本轮不为这两项小幅新增差异启动全面微优化，保留全部数据交独立审阅决定；若要求性能完全收口，该条件仍未达成。

## 分配、布局与证据边界

独立分配诊断中，冷期 EXE-local prepare/late/remount 计数两侧同为5/33/535；Flow Update/Event各三个诊断帧均为0。
实际观测值及 Lua VM计数保留原始CSV；计时区间没有开启分配诊断。EXE-local new 不覆盖全部 DLL 分配，
旧 CSV errors/failures 和不存在的分配字段继续为 null，不能据此宣称零错误或零全局分配。
Flow共同源码observer在计时外检查保留失败记录0、业务计数与退出后的资源0；它不是无界累计错误计数器。
其他旧benchmark以退出码及准确工作量核查，未发明缺失的错误字段。

实际编译器 layout：AwaitableRecord 两侧192字节；ExecutionInstance56→48字节，
新 Event/Timer实例索引各16字节，三数组合计每实例净增24字节。
旧 NextStepWait/SimulationDelayWait各112字节，新统一 Timer Wait184字节，另有delay heap ID8字节；
backing被唯一awaitable同时存活量限制，两个逻辑上限保持不变。
这些尺寸不能代表总内存：还含SlotMap元数据、队列/路由backing及未完全观测的DLL分配。
diagnostics/layout-final 每类型独立编译保留命令和输出，没有修改计时产物。

验证只覆盖本机 Windows/MSVC、实际 LuaJIT 配置与已运行后端；不声称覆盖所有平台/VM、Android或未来调度方式。
新原始证据单独增量归档、逐项SHA-256验证，并从远端固定提交重新下载复核；原SR-2/SR-3包与身份保持不变。
