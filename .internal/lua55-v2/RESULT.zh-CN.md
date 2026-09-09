# Lua55 整链路实施 v2：RESULT

状态：**P0—P8 适用工作已实施并完成本轮验证，提交整体审阅**。不宣称性能等价、任意 Lua 执行形状均已优化或所有平台已获资格；不进入新阶段，不合并 main。

## 1. 身份与可重放入口

| 身份 | 固定值 |
|---|---|
| 用户指定基准源码 | `a79141cbb64b1d23af5f697bb981f84707225b4a` |
| 保留并实际执行的旧 Lua55 资格 | `55dcb3fa85a9a2b7dcc87b178dd9ff76ba75c644` |
| 本轮最终资格、安装、成本源码 | `a6f16d6de6a67f6a4422553d31b42c1ac2b3e4c0` |
| 固定安装 lux-cxx | `3100f54d0743c5ed94a4ccf5943df04e933de255`，`install/q2/c` |
| 固定安装 toolset | `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`，`install/q2/toolset` |
| 唯一产品 VM | Lua 5.5.1 + `lux-leaf-r2`，MSVC 19.44.35228，RelWithDebInfo、/MD |
| 官方 Lua 归档 SHA-256 | `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce` |
| r2 patch SHA-256 | `528bb079282e1093d0570c48b0ce37a2cb38f53b6c985da4e19305e5271c4d71` |
| 独立扩展头 SHA-256 | `c44ea9cb5c4e1aa470e4a1739640c751853741e333a70c7f994aa28a684a0e30` |

当前文档/归档提交不是新的测量源码。实际命令、退出码、源码和产物哈希在
[原始证据索引](../evidence/script/lua55-v2/INDEX.md)；机器可读结论在 [result.json](result.json)。
原始 v1 报告和归档未重标、未重打包。本轮没有运行 Lua54/LuaJIT。

开发在既有隔离克隆 `build/RelWithDebInfo/s5/source`；资格来自独立 clean clone
`build/RelWithDebInfo/script-region-opt/final-source`。复用 `o/w/d`、`o/w/t` 两个构建槽；
当前 SDK、工具、唯一 Lua55 前缀分别是 `install/o/v2/sdk`、`tools`、`lua55`。
固定 cxx/toolset 的 180 + 18 项历史清单文件一致，安装 SDK manifest 明确声明 clean `3100f54d`。
cxx 另有 6 个 LIB 未列入旧清单，本轮单独保存实际哈希，未将它们冒称为已做历史逐项比对；本轮未升级或改写依赖。
未用依赖源仓库的新 HEAD 代替安装身份。
原始 main 的 HEAD 与 7 个未知修改的内容哈希未变。修改过的模块公共头与三个安装头前缀的 21 项核对一致；没有运行 Android 构建。

## 2. 已落地的调用链与子提交

| 子项 | 实际实现和主要提交 |
|---|---|
| P0 | `42ffbf9d4`：删除活动旧 VM 选择、策略、JIT 字段、模板分支、重复测试与当前多 VM 驱动；驱动原件进入历史目录。根配置对 `LUX_LUA_VM` 明确拒绝，不做映射。实际 LuaJIT 旧选项配置退出 1，恢复无选择器配置成功。 |
| P1 | `cea6bbcc` 至 `c611ca0b`：Event/Ability 采用 C 边界→typed C++ worker→C error/yield，删除 Lua wrapper/factory/cache。新 thread、root、函数/self、plain 参数在同一保护段准备；恢复结果转换后检查原执行身份与资格。 |
| P2 | `a459a256`：不可变 CodecPlan/shape、固定成员访问 thunk、VM 字段名 root；plain 值树一次保护转换，不再逐调用构造 plain node、逐字段 pcall 或重复构造 string key。自定义 converter 保留逐字段真实时序。 |
| P3 | `5b708b07`、`4c51bd845`：从 lua_newstate 开始拥有 size-class allocator，预算包括块头；固定 continuation root 槽；实际接入 INC/GEN 和六项 GCPARAM。默认选择 INC，保留上游参数值，缓存 16 MiB。 |
| P4 | `c914ed80`：执行入口定位一次，通过私有 ExecutionAccess 传递稳定实例关系；完成、unlink、Timer 关联复用已定位关系；takeAwaitable 直接删除已找到的记录。小结果仅初始化/移动有效字节。 |
| P5 | `5c7ed115`、`0410e2fcf`、`4c20147d7`：Native ABI v6 显式 context，全部生成/消费迁移；FlowForge 使用真实 CFG 的跨挂起存活、精确初始化、可证明不重叠的 frame 槽复用，以及直接 prepared Event 桥。 |
| P6 | `5bd78a497` 至 `a6f16d6d`：实际修改 Lua CallInfo/OP_CALL/resume/unroll/status 传播，最终 r2 使用独立扩展头和私有类型化 OP_CALL 结果。r1 原件和试验身份保留。 |
| P7 | `c6772bd49`、`6099b189c`、`82701842f`：受保护冷期加载、内容身份缓存、固定函数存储及失效内容回收；与本次相关的 OOM/重载容量缺陷有修前失败和修后成功记录。 |
| P8 | `06617b78` 起补充 Physics 错误/关闭可观测性；完成独立 clean commit 全量、安装/迁址、增量和整体成本验证；静态 FlowForge/ThinLTO 实际纵向尝试有独立记录。 |

普通 Lua Ability 热路径是：已准备 C closure → typed worker 取得当前执行 frame/原调用资格 →
CodecPlan 读取参数 → 必要的转换后原资格重验 → prepared provider → CodecPlan 写结果 → worker 正常返回 → C 边界返回或报错。
输入错误不调用 provider；结果错误不撤销已经发生的 provider 业务。BeginPlay/EndPlay 的专属资格、standalone 与已绑定后失效的区别保留。

Lua 挂起热路径是：固定槽取得 continuation → 保护段创建 fresh thread、root 和参数 → lua_resume →
C 原语的 typed worker 登记来源并返回 → 合格 leaf 正常返回 LUA_YIELD（否则同一 Lua55 标准 yield）→
Execution 关联结果/continuation。来源到达后仍在唯一稳定点按原预算 pop、完成接纳、resume；每次 pop 后使用同 frontier 补接入。
恢复结果的转换可能执行自定义代码，因此转换后不能用先前 ACTIVE 判断继续放行。

七组件所有权保持：Instances 管身份/权限/实例存储，Bindings 管绑定/连接，Preparer 管冷期目录和移交；
Execution 唯一拥有最终结果，EventWaits/Timers/CompletionIngress 只拥有各自来源关系，System 排定跨组件顺序。
没有共享完整 State，没有新 Runtime 聚合者、独立 tick、额外恢复点或预算扩张。

## 3. 资源与安全合同

- allocator 的 size classes 是 32/64/128/256/512/736/1536/4096 字节；超类或缓存预算的块走系统堆。
  原地缩放保留有效内容，失败保留旧块；只复用 VM 已释放的内存。缓存预算不是整个 VM 或进程的内存上限。
- 每次新挂起执行仍创建新的可观察 Lua thread；同一次执行的 resume 使用原 thread。
  固定 root 槽不改变 thread 身份、取消/close、闭包保活或普通 coroutine 的行为。
- CodecPlan 不依赖任意对象 offsetof：非 standard-layout/const 成员使用实际 typed thunk/构造策略。
  owning C++ 临时对象在外层转换 frame 中构造并逆序清理；可能 longjmp 的 Lua 操作放在受保护的窄操作内。
  自定义 converter 后面的字段不提前读取；方向集合、strict raw table 和原错误路径保持。
- 私有 ExecutionAccess 指向固定实例执行存储，不缓存 moving SlotMap 的 awaitable/continuation 裸指针。
  用户代码后仍检查撤权/代次、pin 和取消状态。小值移入恢复 frame 后先释放原逻辑槽，未改变同容量嵌套登记的成败。
- prototype 缓存键同时包含不可变 content identity 与 AssetId 发布域。共享内容不让另一个资产的旧 closure 获得当前能力。
  固定 function 槽保持已发布地址；新内容替代时，仍被实例借用的旧 prototype 保持有效，最后实例释放后回收根与函数槽。
  不同资产顺序复用 backend 时可回收闲置发布域；新资产冷期有界选择闲置项，同 ID/同内容的正常重建不扫描全目录。
  runtime 继续先结束 continuation、释放 prepared method，再销毁 backend 实例和宿主/资产 lease。

新增重载回归保留一个旧 prepared 入口，重载 32 份同资产新 content identity（超过 prepared 容量），
新实例与旧入口各调用 32 次；旧 closure 的 provider 为 0，累计 provider=97，cache_growth=0，最终 prepared=0。
原 32 次同内容重建和冷期 OOM（4 次拒绝增长）也继续通过。该修正没有增加热重载产品 API。

## 4. VM patch、编译器及有限尝试

最终产品启用 `lux-leaf-r2`。快路径限于 resume/unroll 驱动的纯 Lua OP_CALL 调用引擎叶子原语，
typed worker 已返回，当前保护边界、CallInfo、opcode、hook、TBC 和状态均合格。
设置 continuation/ctx/nyield 后保留 C CallInfo，以明确的私有调用结果返回至 lua_resume。
标准错误仍走官方保护。hook、tailcall、TFORCALL、metamethod、中间 pcall/xpcall/C reentry、
close/finalizer 与 continuation callback 再 yield 等未覆盖形状都使用同一 Lua55 的标准路径。
普通用户 coroutine.yield 未修改。没有汇编跳转、Windows POSIX 宏、关闭 CFG/CET 或 Lua C++ 异常方案。

VM contract 实际包含 15 个主用例及深层/重复等待、双 thread 交替、身份/dead 状态、
标准 fallback、错误 recovery、取消/close 和恢复后 OOM 断言。r2 的 smoke、contract、上游 basic 三项通过。
官方测试包 SHA 为 `da07b543872dc0bb2ff12aabd0c248578d78df3eb6b67efdc537a46d455c7f31`。
原包保留；Windows 副本将 /dev/null 换成 NUL，并让 Linux /dev/full 分支遵守 portable 条件，原断言未放宽。
未运行上游内部 C 测试库，不能称上游所有平台/所有测试全过。

GC 只做一次同量默认参数 INC/GEN 对照（r1 中间候选）：Event 由 GEN 1393.108 ns/循环到 INC 1023.860，
Physics 由 GEN 3.700 ms/帧到 INC 3.373。INC 的系统堆请求更多，CPU 却更低，未将分配次数单独当成性能结论。
六项参数保留上游默认，未做无限参数搜索。

实际 clang-cl 19.1.5 C 依赖构建通过 labels-as-values 编译/运行探针、上游 jump table 和对应测试；
该 r1 独立尝试的 Event 为 1126.272 ns、Physics 3.473 ms，未显示替换 MSVC 的依据，最终未采用。
没有把这些中间 r1 数字重标为最终 r2 数据，也没有宣称最终 r2 的 clang 产品已验证。

已有 gameplay.ability 图实际导出 bitcode，与同工具链真实 provider 进行静态 ThinLTO 编译、链接、运行：
status=0、provider_calls=2、value=41、ABI=6。反汇编仍有 wrapper 调用，未证明 graph→provider 间接入口完全消失；
未保留临时导出代码、未改产品链接方式、未建立 Player 框架，也未宣称穿透已编译 DLL。

external strings/fixed bytecode：本轮不接入。现有支持集没有可直接承接外部拥有型字符串的值通道，
字段名已经按 VM root 去重；加载侧仍使用受保护的 source `t` 模式，没有已定义的可信固定字节码 buffer lease。
不为展示新 API 增加 string 类型或新 bytecode 格式。

## 5. 正确性、生成与安装

| 集合 | 最终证据 |
|---|---|
| 独立 tracked guard；Toolchain all、二轮无工作、全量 CTest | 109/109 通过；`qualified-t-*` |
| Developer all、二轮无工作、全量 CTest | 125/125 通过；`qualified-d-*` |
| Lua55 r2 dependency | 3/3；`p8-vm-r2-linkage-*`，最终 dependency 二轮无工作 |
| 当前 SDK 15 个消费者 | 全部通过；保留首次完整生成和最终 SDK 重编译/运行的独立身份 |
| 真实迁址纵向链 | script-lua-values、lua-script-packager 均通过；实际资格源码、d/t 构建和原 SDK/tools/VM 前缀不可用 |
| 值生成增量 | 13 类通过，包括 64 字段界限、预期失败、修改/恢复后真实生成 provider 执行；模板/消费者代码此后无变化 |
| 公共头、固定依赖、工作区 | 21 项同步、198 项依赖哈希、main 7 项保护核对通过 |

最终 SDK 纵向 oracle：legal=2、rejected=2、begin=1、end=1、provider_ctor=1、provider_dtor=1、
lease=1、release=1、backlog=0；另保留字段 id=7、weight=2.5、各阶段独立 provider 次数断言。
迁址编译命令检查没有开发目录、旧 build/generated、pinclude 或 sinclude。
开发克隆被当前会话占用，未声称它也成功隐藏；实际构建使用的独立资格源码已隐藏。

原生命周期、七点重入、同步错误、Bindings 回滚、身份/装配容量、Event/pin/frontier/预算、
Lua/CppStatic/Native/FlowForge 和生成失败清理断言保留。旧六类 Scene Lua 协议在本轮唯一 Lua55 集合中通过；
没有运行旧 VM，也没有用历史结果代替本轮日志。
新增恢复转换回归：立即退休 provider=0；延迟 stop 保留当前批次资格 provider=1；均 roots=1/released=1/backlog=0。

## 6. 整体成本

| 场景 | 基线每操作中位 | 候选每操作中位 | 三组配对耗时变化 | 配对中位变化 |
|---|---:|---:|---|---:|
| Event 完整循环 | 2058.930 ns | 1073.989 ns | -45.16% / -47.84% / -49.37% | -47.84% |
| NextStep 完整循环 | 2129.534 ns | 1267.826 ns | -43.05% / -42.20% / -40.46% | -42.20% |
| scalar Ability | 158.346 ns | 157.086 ns | -5.34% / +0.47% / +2.15% | +0.47% |
| 完整嵌套 record Ability | 1904.676 ns | 1244.396 ns | -33.70% / -35.58% / -30.28% | -33.70% |
| 混合 Physics 帧 | 6.314 ms | 3.482 ms | -47.10% / -44.86% / -44.40% | -44.86% |

scalar 三组仅一组更快，未确认收益或性能等价。每侧独立中位值的比值不等于配对百分比中位数，
该场景两种统计量方向不同，因此保留全部配对而不选择有利数字。

批量总时间（秒，基线→候选，依次三组）：

- Event 完整循环: 38.947959→21.357484；41.178608→21.479788；43.223741→21.883251。
- NextStep 完整循环: 44.543793→25.369798；41.160910→23.790555；42.590670→25.356524。
- scalar Ability: 3.318893→3.141716；3.166920→3.181726；3.058876→3.124614。
- 完整嵌套 record Ability: 1.876968→1.244396；1.914564→1.233372；1.904676→1.328000。
- 混合 Physics 帧: 12.578381→6.653646；12.628770→6.963524；12.671239→7.045185。

主机为 Intel Core i7-13700KF（16 核、24 逻辑 CPU）。固定 10,000 个实例、seed=1592598566、worker=0、原恢复预算 10,000；每项基线/候选各三次独立进程，
顺序 AB/BA/AB，固定同一逻辑 CPU affinity=16。Event/NextStep/scalar 各 1000 次 warmup、2000 个计时帧；
record 为完整嵌套 record Ability 的 1,000,000 次计时调用；混合 Physics 为 300 次 warmup、2000 帧。
四个主场景中的 Ability 分 scalar/record 两条腿，未改工作量、预算、安全或派发顺序。
record 在一个真实 runtime 方法中的 Lua 循环内逐次调用 provider，归一化粒度与 scalar 的逐实例 Hook 调用不同。
24 份帧级 CSV 还逐帧核对了双方业务字段、调用/恢复计数递增；见 work-oracles.json。
批量总时间、逐对分布、p50/p95/p99 帧时间、实际调用/恢复/等待、积压和错误范围在 `final-costs/runs.json` 及 CSV。
record 只有批量计时，未编造单调用尾分位；恢复延迟保留 step 协议测试，没有新增纳秒级 latency 采样。

Event 的独立 script readback 在计时外核对全部实例：30,000,000 次含 warmup 的完成，每实例 93001，checksum=930010000。
NextStep/scalar 有累计调用/provider/恢复与错误/关闭检查，没有另加逐实例值读回；如实保留该可观测范围。
Physics 旧基线只检查每帧保留错误，旧 shutdown 未检查累计错误，因此旧 errors 保持 null；
新候选另有累计错误和完整关闭检查，未将旧 null 改为零。
所有差异都针对完整操作；Event/NextStep 总帧时间不能全归给 resume，也不能把整体差值完全归给 VM patch。

独立 Event 分配诊断（同样含 1000 次 warmup + 2000 次计时帧，但不把诊断耗时用于上表）：

| 观测 | 基线 | 候选 |
|---|---:|---:|
| VM 逻辑 allocation 请求 | 150,090,358 | 120,090,488 |
| VM 逻辑 realloc 请求 | 161 | 88 |
| fresh thread 创建/恢复/释放 | 30,000,000 / 30,000,000 / 30,000,000 | 30,000,000 / 30,000,000 / 30,000,000 |
| 累计请求字节（非同时占用） | 33,850,700,156 | 32,650,510,303 |
| 进程 peak working set | 98.598 MiB | 108.586 MiB |
| 进程 peak private commit | 104.164 MiB | 109.750 MiB |

候选直接记录到 heap_alloc=44,791,478、cache_hits=75,299,027、in_place=71、failures=0，
peak_live=45.310 MiB、peak_retained=16.000 MiB。
leaf-return=30,000,000，标准 leaf fallback=0（该 Event fixture 的合格路径；复杂形状由 VM contract 单独覆盖）。
基线没有同口径的独立 heap_alloc/cache 计数；它们保持未观测。上述 VM 请求数包含各版本实际 instrumentation 的启动覆盖，
不假称是 ROI-only 或系统调用数。计时 CSV 的 VM/EXE 统计关闭项也不把原始零字段解释为零分配。
Physics 的 EXE-local allocation 计数实际开启且为零，范围仅限该 EXE 的 operator new，不覆盖 DLL/VM/系统堆。

候选多用了约 9.988 MiB 峰值工作集、
5.586 MiB 峰值 private commit。
这是已观测的内存代价；没有把“减少 allocator 往返”表述成“没有对象分配”或“整体内存下降”。

计时、VM 分配诊断、此前 compiler/GC 尝试分开运行。历史成本债务和本轮仍未分解的开销继续存在；
本轮不宣称性能等价、残余均是必要安全成本或已达到某个 invoke 占比目标。

## 7. 无效试验与限制

保留失败原件和退出码：测试初版预期错误、混用重编前 EXE 的无效诊断、源码格式门禁、
r1 patch 换行问题、旧 ABI v5 Physics artifact、未初始化 VS 链接环境、上游 Linux 设备路径、
Windows 适配初版转义错误、扩展头初版 C++ linkage 错误、P7 容量负例及其后发布域/格式修正。
旧 v5 loader 是正确拒绝；benchmark 解引用失败 expected 的问题已窄修，v6 artifact 已用真实生成器重建。
这些试验不参与有效成本比较。

迁址首次 PowerShell 移动遇到隐藏 .git/会话目录占用，造成开发克隆部分移动；
逐文件按哈希恢复后两个克隆 HEAD/工作区一致，随后用同父目录原子重命名完成实际资格路径隐藏与恢复。
清理空临时目录的操作被自动审批拒绝（仅返回 blocked by policy），已保留该空目录；没有绕过删除限制。
无效迁址与恢复清单保留在原始证据中。

支持矩阵：Lua55 的现有 scalar、有限 enum、opt-in 嵌套 record，普通 typed Ability 双向、
既有 export record 输入和 Event 输出；自定义规则遵循方向集合及构造/清理合同。
sol2 对象绑定保持独立。没有新任意对象图、共享可写 record 内存、非平凡跨挂起结果或线程池语义。
Native 只接收 ABI v6，v5 必须重新生成；序列化 schema/wire 与脚本业务值不变。
引擎原语成为 C 函数，内部 Lua wrapper 调试栈消失，替换 coroutine.yield 不再拦截引擎原语。

已验证环境是上述 Windows/MSVC/RelWithDebInfo 配置；Linux、Android、其它编译器的最终产品、
上游内部 C VM 测试与完整引擎游戏项目未获本轮资格。没有重新启动多 VM/五对微优化矩阵。
交付后等待统一审阅，不合并 main、不发布 tag、不冻结框架、不扩功能。
