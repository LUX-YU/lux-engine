# SR-5 权限补正与有限成本检查

## 1. 范围与身份

本轮执行《SR-5 权限补正与有限成本收口任务单》。保留首批转换、生成链、owner、调度和生命周期，
不扩类型，不重做 SR-4，不进入 SR-6，不合并 main。审阅方依据源码指出缺口；下述复现与运行证据由实施方取得。

| 身份 | 含义 |
|---|---|
| ebf081158 | 本轮核对的本地/远端入口 |
| C0：6086e4a4 | 已交付 SR-5 生产/资格与本轮主要成本参照，旧产物保持不变 |
| H：cd7160ff | 历史已支持路径参照，不给其旧无保护输出改安全标签 |
| 31dfc9ca | 仅增加正式生成 scalar/零参数探针与独立复现；生产仍为 C0 |
| c29aa8d5 | 独立正确性修正提交：每次初始准入与 standalone 区分 |
| C1：f64caddebcc353c07ac03637135f99586986eb6e | 权限修正加两项有收益窄改；最终生产/资格快照 |

实际入口工作库位于 build/RelWithDebInfo/s5/source，新的独立 clean clone 与构建/日志位于 s5c。
main 实际 HEAD、全部未知文件内容及依赖以 start-identity.json 固定，不照抄旧工作区条数。
lux-cxx 3100f54d、toolset 99c3d048 与 install/q2/c 保持；C0 DLL/EXE 哈希开工留存。
原 ee4322d1 包及其 1,763 项哈希记录不修改、不重复上传。

## 2. 资格契约

`LuaAbilityProjectionAccess::current()` 是本次入口：先验证原 Lua thread/frame、prototype layout、prepared slot/context/dispatch，
再取得当前 core 资格。零参数、默认 scalar、custom/record 以及原 async scalar 都经过此入口。
`can_reenter` 只控制参数转换后的原资格/原 frame 复验，不再控制是否需要初始准入。

`ScriptBehavior::hasInvocationAuthority()` 仅回答窄连接是否装配；它不回答是否 ACTIVE，不是另一套可写权威。
已装配而 capture 无效必须拒绝。原 ticket 仍由 Instances 检查完整实例、retirement epoch、状态/停止准入和生命周期类别。
不能用 isAttached、失败日志、曾出现的错误或另一个 active frame 放行。

真正 standalone 指 descriptor 调用没有安装 core authority（空 behavior 或未装配的 behavior）；它仍须通过 backend
原 thread/frame/layout/prepared 检查，寿命由直接调用方管理。带有无效 core authority 的旧 frame 不属于 standalone。
转换后复验还比较原 authority 装配类别、behavior、execution、context、dispatch 和 local slot，不能切换借用目标。

BeginPlay/EndPlay 继续使用 Instances 的受控窗口，不能一律要求 ACTIVE。普通捕获失效不会转换成生命周期资格。
纯默认 scalar 读取没有用户代码/GC/析构重入，初始成功至 provider 的连续窗口可以复用该资格；不跨 Lua pcall 或用户代码缓存 ACTIVE。
async 也经过 current 的初始检查，其既有 invokeScriptAbilityAsync/Awaitable admission 仍独立保留，没有新增 async 类型。

## 3. 独立复现与真实回归

正式 LuaValueTestAbility 新增 test-only scalarProbe(i32) 和 zeroArgumentProbe()；没有测试专用生产 bypass。
每项独立进程都输出 ADMISSION_BEGIN、Lua 后续分支 reached/ok、实际 provider 计数；第一个断言失败不妨碍其他子例独立运行。

| 子例 | 31df 修前实际结果 | 正确性修后预期与已运行结果 |
|---|---|---|
| retire-scalar | reached ok=true，scalar=1，计数断言失败 | reached ok=false，scalar=zero=0 |
| retire-zero | reached ok=true，zero=1，计数断言失败 | reached ok=false，scalar=zero=0 |
| stop-scalar | shutdown 在原保护内 busy；scalar=1，计数断言失败 | busy 保留；scalar=zero=0 |
| stop-zero | shutdown 在原保护内 busy；zero=1，计数断言失败 | busy 保留；scalar=zero=0 |
| input-recovery | 未退休，捕获普通输入错误后 scalar=zero=1 | 仍为1/1，非永久 error latch |

四个修前进程均在真实计数断言处终止，退出码 -1073740791；不是故意触发悬空回调。对照退出0。
修后五个独立进程均退出0；每个实例 BeginPlay/EndPlay 的 angle provider 总数为2，最后 backlog=0。
四项错误被 Lua 捕获后此 fixture 的顶层 failures=0，进一步证明判断不是读取失败日志。
已有其他同步退休 status32 检查保留，不能将此 fixture 的结果外推成所有退休调用都不记录错误。

同 VM 合法嵌套、const 输入、无默认构造/第二参数失败、结果失败 provider=1、push-only 拒绝、OOM/frame/error fallback
仍在原真实测试中。新增 standalone 同一生成入口分别使用 null host 和未绑定 host，scalar=2、zero=2、custom=2。
现有 closure_provenance 还检查另一 prototype 的旧 closure 被拒绝、同 prototype 合法关联，以及销毁重建后旧 closure 不取得新能力。
没有把 sameIncarnation 扩成重新获准；七点清理重入、移动 busy、Timer/pin/frontier/预算及 Scene Lua 继续执行原断言。

最终 C1 仅 RelWithDebInfo：Toolchain 全量108/108（45.55s）、Developer 全量123/123（45.03s）、
Lua54 受影响集合110/110（41.68s）；均运行 all -j4 -- -k0、安装及第二轮 no-op。
原15个安装消费者全部通过，13项增量检查按预期完成。没有新增 CTest 注册数；原 runtime 测试增加独立场景，
完整 JIT/解释执行变体也运行这些场景。最终两种 VM 各5个独立命令另存 final-cases，均退出0。
Lua54 不是全 profile 测试声明；六项 Scene Lua 测试保留实际资产/既有逐 step 断言，本轮未认领旧修复。

core hasInvocationAuthority 不增加 ScriptBehavior 字段；Lua C++ projection access 增加配置类别位，相关 C++ 消费者需重编，
已通过实际安装消费者验证。generic backend 操作表/签名、wire/schema 与首批类型范围没有变化。

## 4. 有限成本实验与安全条件

实验前在 cost-plan.json 固定顺序、工作量和保留条件：五对独立进程交替先后；不删除慢对。
scalar 使用10,000配置、5 warmup、3,000计时帧，即30,000,000实际 provider 调用；seed=1592598566、原预算2,000。
两侧对称延长原短试验。record 为两个相同字段、1,000 warmup、1,000,000次输出，VM 默认 GC 不关闭。
计时、分配/保护计数和故障注入分开。

只验证两项窄代码实验：

1. scalar 准入结果：优化产物显示 current 先在局部构造96字节访问结构，再整块复制，其中 ticket 先清零随后又被有效捕获覆盖。
   实验按字段直接写入输出，不删除 capture、valid、转换后 revalidate 或 prepared 查验。
2. record 操作错误区：C0 的 table/setField 机器码存在对256字节路径的 memset，即使该操作只返回 bool。
   实验把只属于 shape 读取的错误对象留在外侧 shape frame，Operation 仅借用其指针；输出仍经过原三次受保护操作。

相邻 capture 和 valid 检查没有在本轮被删减。scalar 的初始检查及 custom 转换后的完整重验都保留，未添加无依据 inline/LTO/PGO。
两项都达到预先固定的保留条件，5/5配对改善：scalar 对纯正确性快照中位 -7.50%，范围[-8.37%,-6.00%]；
record 对 C0 中位 -5.92%，范围[-7.24%,-4.68%]。没有继续尝试其他存储/inline/VM组合。
实验开发产物由实际 EXE/DLL 哈希及 trial.patch 绑定；其编译内嵌提交字段按原样保留，不能冒充最终 clean C1。
随后独立 clone 快进到 f64cadde，重新 all/测试/安装和最终配对，最终结果另列。源码有少量指令不能自动推出净收益。

ProtectedRecord.hpp 是独立诊断：在一个正确初始化的 C trampoline 中，只借用 CollisionEvent 的两个 scalar 字段，
没有 owning C++ 局部对象或 noexcept 的 Lua 非局部退出边界。表创建、键压值、rawset 位于一次 pcall；
外侧 Lifetime 对象验证中途错误/分配失败返回后仍存活，退出外侧 frame 才析构。成功+1，失败恢复原 top。
它不调用 custom rule、不读取/构造 record、不作为生产 fallback，也不能证明任意生成递归或用户规则可机械搬入该边界。

实际单独 hook 计数：10,000次输出，正式生成30,000次受保护 C 调用，手写诊断10,000次；计时运行不启用该 hook。
两种 VM 的中途错误/OOM/外侧寿命/恢复检查通过；完整日志含 point=1/2、top=0、live=1，最后 live=0。
因此 H 无保护手写与正式生成的总差距不等于“代码生成开销”。保护粒度、错误处理和实现选择分别报告。
生产没有合并 pcall：把一般递归/custom 规则证明为纯非拥有内部操作需要更多协议，不为满足假设新增另一套正式路径。

### 最终 clean C1 配对

下表总时间是各侧五进程中位（ms）；单位成本均摊完整批次，不能称为单独 provider/resume 函数耗时。
配对变化是五个独立差值的中位，不能用两侧中位之差替代；完整逐帧业务核验及所有分布保存在 validated-costs.json。

| C0→C1 场景 | 计时有效量 | C0/C1 总 ms | C0/C1 ns/单位 | 配对中位变化 | backlog |
|---|---:|---:|---:|---:|---:|
| cpp-update-10k | 50,000,000 actual_new_calls | 226.3003 / 219.2064 | 4.5260 / 4.3841 | -2.075% | 0 |
| lua-update-10k | 50,000,000 actual_new_calls | 1336.7675 / 1312.7738 | 26.7354 / 26.2555 | -0.178% | 0 |
| lua-ability-10k | 30,000,000 provider_calls | 2261.8731 / 2454.1162 | 75.3958 / 81.8039 | +9.151% | 0 |
| lua-coroutine-10k | 10,000,000 actual_new_calls | 9716.3154 / 10522.6232 | 971.6315 / 1052.2623 | +8.099% | 8000 |
| lua-event-10k | 50,000,000 actual_new_calls | 52631.5401 / 51433.6639 | 1052.6308 / 1028.6733 | +0.089% | 0 |
| flow-update-10k | 50,000,000 actual_new_calls | 418.1540 / 413.4645 | 8.3631 / 8.2693 | -1.842% | 0 |
| flow-event-10k | 10,000,000 actual_new_calls | 5447.8353 / 5411.3242 | 544.7835 / 541.1324 | -3.182% | 8000 |
| event-fanout-10k | 10,000 completions | 1.9901 / 1.9822 | 199.0100 / 198.2200 | +0.081% | 0 |

五对完整分布，按预先排定的 pair 0—4；Δ 为整批 C1−C0 毫秒：

| 场景 | Δ ms | Δ % |
|---|---|---|
| cpp-update-10k | -17.2044, +6.2935, -1.9486, -8.5556, -4.6953 | -7.454, +2.871, -0.926, -3.756, -2.075 |
| lua-update-10k | -50.4206, +66.1323, -2.3442, +22.8196, -107.8097 | -3.520, +5.334, -0.178, +1.707, -7.968 |
| lua-ability-10k | +126.8687, +176.6315, +207.9628, +218.6647, +217.1088 | +5.521, +7.818, +9.151, +9.667, +9.705 |
| lua-coroutine-10k | +1029.2939, +788.3976, +95.7920, +477.6902, +920.8102 | +10.671, +8.099, +0.948, +4.984, +9.477 |
| lua-event-10k | +8.3298, +7239.7073, -2821.2776, +947.3149, +46.8057 | +0.016, +16.762, -5.200, +1.797, +0.089 |
| flow-update-10k | -9.2789, +8.7281, -10.1831, -7.7011, +11.6916 | -2.196, +2.095, -2.404, -1.842, +2.814 |
| flow-event-10k | -256.2740, +25.8552, -193.8911, -177.8431, -79.1773 | -4.481, +0.479, -3.559, -3.182, -1.471 |
| event-fanout-10k | +0.0406, -0.0185, +0.0016, -0.1648, +0.0022 | +1.917, -0.930, +0.081, -7.642, +0.111 |

C0→C1 scalar 的配对增量中位 +6.9321 ns/provider（+207.9628 ms/30M）；它包含补上的完整初始资格，
而 C0 对默认 scalar 跳过该检查。仅修正→窄改实验的改善不抵消最终 C0→C1 +9.151%，不改写安全参照。
Lua coroutine +8.099%（配对中位 +788.3976 ms/10M、+78.8398 ns/实际新调用）也单独登记。
该路径的 async scalar 现在经过初始 core 准入；源调用边界解释了新增工作，但本轮没有独立分离其全部时间，
故不将整批差值全部归给资格或 resume，也不认定为已证明必要成本。其8,000 backlog与C0相同、由原预算保留。
其余方向混合且部分 Event 配对波动明显；保留全部分布，不声称小幅中位差代表稳定优化。

H→C1 scalar同量30M：1982.4332/2458.9425 ms，
66.0811/81.9647 ns/provider；配对中位 +25.648%，
五对百分比 +25.648%, +21.695%, +24.037%, +27.084%, +26.258%；
五对Δms +501.8872, +428.7824, +476.5093, +545.2759, +523.4761。
配对增量中位 +16.7296 ns/provider，backlog=0。首次H复跑的开发环境初始化出现命令行过长警告，
所有记录保留于 performance-H-C1，但不进入最终统计；新进程 performance-H-C1-clean 无警告且五对全部纳入。

旧benchmark缺失的 errors/failures仍为null。FlowForge的计时区间外 INTEGRITY retained=0 与关闭清零另存，
它们不是所有错误的计数替身；真实资格负例的provider=0、结果错误provider=1由独立 correctness 运行证明。

### 两字段输出与新值绝对成本

| 路径 | 1M次总时间中位 ms | ns/输出 | 独立10k诊断 protected C调用 |
|---|---:|---:|---:|
| H旧无保护手写 | 99.6799 | 99.6799 | 0 |
| 同安全目标手写诊断 | 122.5410 | 122.5410 | 10,000 |
| C0正式生成 | 254.8304 | 254.8304 | 30,000 |
| C1正式生成 | 240.0040 | 240.0040 | 30,000 |

C0→C1五对Δns/输出：[-11.7151,-13.7756,-16.5632,-14.0426,-10.8872]，中位−13.7756；
百分比[-4.6540,-5.4741,-6.4997,-5.4190,-4.1781]，中位−5.4190%。
H→C1配对增量中位+140.3241 ns；H→protected中位+22.8525 ns；protected→C1中位+116.4941 ns。
所有H/protected/C0/C1逐对总时间、差值和百分比分布见 record-final/validated-costs.json。
不把+140.3241 ns全部叫作生成开销；受保护诊断证明特定两个scalar输出可用更粗保护，但不证明通用生成协议等价。
C1仍比该诊断贵约116.5 ns/输出（配对中位），这是可量化残余，不是性能等价或全部必要安全成本。

全部输出计时errors=0、backlog=0、fields=2、完成量1M。LuaJIT计时与分配/调用hook诊断分离；
独立10k输出C0/C1均20,081次Lua分配、1,122,592 bytes，保护次数均30k；EXE-local分配不是全DLL堆统计。
Lua54只做对应保护/错误/清理诊断，未宣称与手写pushinteger的数值子类或性能等价。
typed Pose五次100k绝对测量中位129.5654 ms、1295.654 ns/provider，errors/backlog=0；
另一次启用VM计数的100k诊断为800,000次Lua分配；该诊断耗时不混入普通计时。

最终反汇编确认C1 table不再调用256字节memset，栈预留由170h降为78h；current直接写访问字段，
仍保留capture调用、40字节ticket拷贝与valid调用。verified-windows.json给出精确符号和行号，不能把未删除的操作记作收益。

## 5. 支持矩阵、限制与交付

首批矩阵保持：bool/i32/u32/float/double、有限 i32/u32 enum、opt-in 有限 record、自定义整体表示及原同步/输入输出方向。
未更改字段生成模板、类型选择、strict raw table、const 构造策略、深度/容量门槛、结果所有权或非平凡 async 限制。
同一 codec/policy 决定表示，保留 sol2 对象绑定的独立职责。

旧 SR-3/SR-4 Event 债务和 C0 已记录成本仍有效。本轮新增成本另列，不用旧债务豁免，不宣称所有残余均必要或等价。
缺失错误字段仍为 unknown/null；关闭计数时的零不是零分配；配对差中位、两侧中位之差、百分比中位分别计算。

新证据位于 [evidence/script/sr5/admission/README.md](evidence/script/sr5/admission/README.md)：独立修前/修后、clean正确性提交及C1资格、原始配对、无效环境试验、诊断源码/反汇编、产物/安装身份与哈希。
本轮未改L0公共头或生成模板；无须新增三个旧install前缀的头同步，新的Engine窄公开入口由三份C1安装及真实消费者校验。
结束时逐项复核main七项未知文件哈希、main HEAD、两个依赖仓状态及全部冻结C0 EXE/DLL，均与开工相同。

建议：正确性与本轮有限调查可以提交验收；不建议在尚未明确接受scalar、coroutine新增成本及record残余前无条件进入SR-6。若统一审阅接受上述已量化成本与未完全分离的coroutine归因，可另行批准SR-6；本轮不继续优化或自动启动下一阶段。
