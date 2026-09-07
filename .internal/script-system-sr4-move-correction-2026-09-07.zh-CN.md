# SR-4 外层移动替换补正与 SR-5 设计交付

日期：2026-09-07。范围：修复 ScriptSystem 移动赋值的继承生命周期缺口；SR-5 仅设计。

## 1. 身份与实际修改

起点为已审阅分支 `22304336806c70b209ae90e585cb5cb0c085bade`，运行时与已验证 SR-4 `1750ce854967a382ee89accf3a3534a0628396cc` 相同。
本轮在 build/RelWithDebInfo 下的隔离源码实施，并用独立 clean clone 验证；没有重新建立 CodeRepos 顶层 lux-engine 子工作树。
原始 main `f0e8c3fd7ae2a39ac4f96692ab089d151514c40c`、五个 tracked 修改和两个 untracked 文件按开工哈希核对，保持原状。
不操作 main，不升级依赖。lux-cxx 固定 `3100f54d0743c5ed94a4ccf5943df04e933de255`，toolset 固定 `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`，使用 install/q2/c、install/q2/toolset。

生产修正只有 [ScriptSystem.cpp](../engine/domain/simulation/builtin/script/src/ScriptSystem.cpp)::operator=(ScriptSystem&&)：
自移动直接返回；有未关闭的目标 State 时先执行既有 shutdown；失败沿现有析构策略 terminate；成功后才移动 unique_ptr。
空目标、已关闭目标不再次执行关闭。源 State 没有交换到临时对象、重建或重新 BeginPlay，稳定地址和完整实例身份随所有权转移保留。
公开 noexcept 签名、析构函数和所有内部 owner 均未改变。调用方如需可恢复错误，仍先显式 shutdown 并处理返回值。

此 default move assignment 在 SR-4 前 `ba6c7be44797e6e7efe4a272d0f208d247f7366c` 已存在，属于本轮发现并复现的继承缺口，**不是 SR-4 新回退**。
没有改变执行、来源取消、恢复预算、frontier、Lua 转换、backend ABI 或逐帧热路径。

提交顺序：`7c37bac2` 新回归 → `4fdb57e3` 测试 optional 解包编译补正 → `a1a1ec6d` 生产修正及 busy 子进程 →
`c8e642c4` SR-5 设计 → `cd7160ff` 精确匹配既有 STOPPING 返回。最终可构建验证源码为 **`cd7160ff47c8f8bf36a44f7fbbdeb813fabb018b`**。
后续证据、报告提交不再修改生产代码或测试。

## 2. 修前复现与旧断言的新增覆盖

修前用 `4fdb57e3` 新 fixture 编译独立 RelWithDebInfo consumer，链接上一轮固定 SR-4 安装 DLL。
这是本轮新执行的复现，不声称重新构建整个历史参照。
旧 simulation_script DLL SHA-256 为 `52882acf52d4fb17dce9512f7a0afe814e9da480ec28f810c18c6899919eb099`；
复现 EXE 为 `9f41dbddf56997f7c51782302bb23d32ac5405bc3cde7f8b60d45dd6e739b329`，路径、链接命令和身份见证据 before/。

实际输出 `MOVE_REPLACEMENT suspended=0 old_end=0 release=0 destroy=0 lease=0 frame=0`，随后在 `old.backend_state.ends == 1U` 断言失败，
退出 `-1073740791`。先打印并刷新资源计数，失败发生在任何旧 endpoint 再分发或 Registry 销毁之前；没有依靠 UAF 作为唯一复现。
修前停在第一个同步场景，不能声称后续异步、自移动和 busy 场景也在旧实现上跑完。

所有测试仍走实际 ScriptSystem，复用 lifecycle fixture 的可计数 backend；两套 Registry、endpoint、backend、artifact lease owner 始终比两个 runtime 活得更久。
没有用独立 unique_ptr 玩具模型替代运行时。

| 新入口 | 独立预期及其对应协议 |
|---|---|
| `testMoveReplacement(false)` | 旧 EndPlay 1、prepared release 3、backend destroy 1、lease release 1；固定序列 EndPlay→3 次 releaseMethod→backend→lease；旧 Hook 回调 0，旧 Registry 销毁安全 |
| `testMoveReplacement(true)` | 两边各通过真实 external awaitable 登记挂起；旧 continuation 先销毁 1 次，再按上述顺序结束实例；旧 completion 返回 STOPPING，旧 resume 0；转移后源 completion 正常接纳且 resume 1 |
| 同一入口的转移与最终析构 | source 完整实例 ID、ACTIVE、host/prepared 地址不变，BeginPlay 仍 1；moved-from shutdown/析构不清理 transferred State；新目标继续 Hook 调用至 2；最终 incoming EndPlay/backend/lease 各 1，异步 continuation 总析构 2 |
| `testMoveEmptyClosedAndSelf` | 空目标接管、活动自移动后业务调用 1、已关闭目标再次接管、空源覆盖活动目标、空自移动；无重复 EndPlay/release/lease |
| `simulation_script_move_busy_test` → `--move-busy` | 真正 Hook invoke 保护内覆盖当前 runtime；terminate handler 必须见旧 release/EndPlay/backend 都 0，源仍 BeginPlay 1 且未销毁；精确退出 73 和 `MOVE_BUSY terminate retained=1` 才通过 |

trace 用手写事件序列核对，不仅比较两个实现的输出是否相等。修正后才执行旧 endpoint/Registry 安全检验。
`move_busy_test.cmake` 是本例有限子进程检查，不引入通用死亡测试框架；非法调用位置不新增延迟回收或等待能力。
原生命周期失败/七点清理重入、Event/pin/frontier/预算、Timer 取消、身份和装配测试的断言与入口没有被替换或放宽。

关闭整个 runtime 时 ExternalCompletionRing::push 先检查 closed，返回 **STOPPING**；单实例旧 ticket 在开放 transport 中才适用 INVALID_ID。
首次候选测试误写 INVALID_ID，两个 profile 都在该新断言失败；精确改成 STOPPING 后重验，生产协议未变。

## 3. 本轮验证与原始记录

最终 clean clone 源码 `cd7160ff`，所有构建与测试串行，仅 RelWithDebInfo，`target all -j 4 -- -k 0`。
Toolchain 全量 CTest **108/108**（46.25 s）；Developer **120/120**（46.03 s），均退出 0。
两套 tracked snapshot 检查、all 构建、install 成功；CMake 改动后的第二轮均 `ninja: no work to do`。
这些是本轮实际结果，不能与上一轮 107/119 混用；新增的一个 CTest 是 busy 子进程，其他新用例在既有 lifecycle executable 内执行。

Developer 六个 Scene Lua 组合本轮继续通过。当前实际 portability artifact SHA-256 为
`D03C4A67CF102C2A404CA756A8CC3D11D4ECADE49CC9418DDB88CFEA147318EC`，来自本轮 Toolchain 生成输入。
LastTest.log 中每组有 step1—5 及同 step 重复稳定点轨迹：step4 resumes=2、delay=1、value=1；step5 resumes=3、delay=0、value=1234。
这六项是已证明并保留的旧预期修正，本轮没有新增 Lua 失败，不再列为“历史未定位”。不把本机 LuaJIT 结果外推至所有 VM/编译器。

无效或中间试验逐项保留：

1. 新测试最初未解包 expected 内的 optional，编译失败；测试单独修正，未作为 runtime 证据。
2. 首次较长 build 路径导致 MSVC 生成 CppStatic obj 报 C1083/Invalid argument；未运行旧 EXE。Developer 因缺少该 Toolchain fixture 阻塞。
   改用独立较短 m4 路径完成 all；无生产源码变更。
3. `a1a1ec6d` 两套 all、install、第二轮 no-op 与 busy 子进程成功，但 lifecycle 在新写错的 INVALID_ID 断言失败；
   原始日志保留在 iteration-return-error。`cd7160ff` 按原有 closed ingress 协议改为 STOPPING 后重新 all/CTest/install，不掩盖中间失败。
4. lua-script-packager 与 script-authoring 首次配置优先找到本轮 Toolchain 的不含 Lua simulation 包；CMake 不跨前缀合并同包组件。
   保留失败配置日志，只将这两项放到新目录，以本轮 Developer 前缀优先重试，生成、运行及第二轮 no-op 全通过；没有源码修改或旧安装 fallback。

最终 **14/14 installed consumers** 通过，authoring 的独立 save/load 也成功。实际链接闭包核对 direct runtime 无 World/Scene/Process，描述 leaf 无 Process/Scene runtime；
安装目录未泄漏 Instances/Bindings/Preparer/Execution/EventWaits/Timers/CompletionIngress 等私有头。
生成器、安装头、DLL 与消费者 EXE 的实际 SHA-256 随包提供。资格 manifest 保留 a1 中间失败，最终汇总只选择 cd 的两项以及上述两项有明确身份的重试。

本轮原始归档、固定提交下载入口与逐文件哈希见 [证据索引](evidence/script/sr4-move/README.md)。旧 SR-4 证据包没有修改或重新标记。

## 4. 保留的成本与限制

本轮没有重测历史性能，也没有用移动赋值修正宣称性能等价。继续保留上一轮记录：
Flow Event E0→E1 五对中位 −2.27%，H0→E1 仍 +11.14%；Lua Event −1.88%；
Lua Update +1.52%（约 +0.3912 ns/实际调用、+0.0039117 ms/本例帧）；prepare +7.61%（差值中位 +10000 ns）。
Lua Update 与冷期新增成本未独立归因，不称为噪声或必要安全成本。
ExecutionInstance 56→48 bytes 加两个各 16 bytes 的来源索引，合计 +24 bytes/实例；旧 Timer 记录各 112、新统一记录 184 bytes，另有 heap/SlotMap 元数据。
这些是既有记录，不是本轮新测量或总内存百分比。

移动赋值回归用可计数真实 runtime fixture，并由真实 Lua/CppStatic/Native/FlowForge 联合测试补充后端覆盖；不声称每个 backend 都单独执行了双 runtime 移动用例。
busy 子进程覆盖当前 invoke 保护导致的失败；没有逐一注入所有可能关闭错误。
本轮不扩展 Android、Lua54 或全仓 RAII 审计，不运行全部历史性能矩阵；没有公共头或转换实现改动。

## 5. SR-5 设计交付与停止点

主设计：[C++↔Lua typed 值转换设计](script-system-sr5-value-conversion-design-2026-09-07.zh-CN.md)。
已核对实际 Lua export、typed Ability 投影、Event owned payload、异步结果、手写 record push、lux-cxx facts、模板及安装生成链路。
首批建议 scalar、有限 enum、opt-in 嵌套 record；typed 普通 Ability 优先双向，既有 record 输入/Event 输出接统一规则；
值转换、对象绑定、跨挂起所有权分别约束。字符串、Entity 新接口、非平凡异步结果与 erased record 返回不自动承诺。

待统一审阅的三个决定是首批范围、strict raw table/缺失与未知键/枚举/边界策略、统一 typed override/generated 体系及旧入口一次迁移删除。
具体构造/失败、Lua 保护边界、用户规则、递归依赖、字段改变失效和隔离安装验收见主设计；这些提案没有实现为生产代码。
本轮停在 A 补正与 B 设计交付，不进入 SR-5 编码或 SR-6，不合并 main、不冻结框架。
