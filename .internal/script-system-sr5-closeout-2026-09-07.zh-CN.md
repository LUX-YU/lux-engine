# SR-5 首批值转换：代码、验证与限制

## 1. 交付身份与结论边界

本轮实际实现首批值转换，停止于 SR-5，等待独立审阅；未进入 SR-6，未操作 main，未更改依赖。
SR-4 移动赋值补正保持原状。完整接口、构造和支持矩阵见
[实施契约](script-system-sr5-implementation-2026-09-07.zh-CN.md)。

| 身份 | 提交/用途 |
|---|---|
| 用户批准入口 | `764fc5d3da2e1e6b85270fd1ffe1cda2d4093971` |
| 本轮实际重新测量的参照产物 | `cd7160ff47c8f8bf36a44f7fbbdeb813fabb018b`；与入口的 engine/modules/cmake/CMakeLists 生产差异为空，原记录未改标签 |
| 主实现 | `b7bf2495`，值边界、typed 生成、真实后端与消费者迁移、原资格重验 |
| 安装/探针 | `ff45d63e`，实际安装失效验证与独立输出成本探针 |
| 语义与错误限制 | `4fe3565e`，语义指纹、有限错误复制及 frame 费用修正；初次完整性能候选 |
| 最终生产候选 | `6086e4a4dfd337ddad522a7a71c7b7d14b571189`，错误存储延迟构造、已知正索引省去重复规范化 |
| 依赖 | lux-cxx `3100f54d`、toolset `99c3d048`，`install/q2/c` 与已核对安装身份 |

候选资格来自独立 clean clone `build/RelWithDebInfo/s5/candidate-source`，构建/安装 `s5/q/{t,d,l}`。
每个资格轮次先检查 tracked snapshot；最终 q 日志绑定 6086e4a4。早期轮次的日志和二进制哈希另存，
qualification.json 追加而非覆盖身份。报告/汇总脚本/证据的后续提交不改变该生产快照。

原 `lux-engine` main 为 `f0e8c3fd`，五项 tracked 与两项 untracked 文件按开工 SHA-256 复核原样保留。
本轮隔离源码位于 build 下，没有重新建立 CodeRepos 顶层 lux-engine 子工程。模块公共头按仓库约定同步
Debug/include、RelWithDebInfo/include、Android/lux-engine/include；这不是 Debug/Android 构建资格。

## 2. 实现与安全边界

值规则位于现有 `script_lua` 模块，backend 只持冷期准备的操作描述。ScriptExecution、EventWaits、Timers、
CompletionIngress 没有转换数据或新职责。原 TaskGraph、稳定点、frontier、预算、single-flight、取消与回收顺序不变。

`LuaValueCodec<T, Policy>` 选择一套完整表示，包含其方向集合；override 的 push-only 不补用 generated reader。
正式 `LuaRecordMarshaller` 类型、backend config 和索引已删除，两个 CollisionEvent 实际消费者迁到生成入口。
仅独立历史成本探针的旧 SDK 编译分支引用旧类型，用于重放旧操作，不是兼容生产入口。sol2 对象绑定未删改。

原 backend 调用保护内捕获 `ScriptInvocationValidity`。参数规则可能重入时，在 provider 前验证同一完整身份、
retirement epoch、原调用类别和执行帧/prepared 关联。退休/关闭后的普通资格不会由 current() 或 isAttached 恢复；
实际 BeginPlay/EndPlay 的独立受控窗口仍合法。C++ ScriptBehavior 的内部关联布局有变化，generic backend C ABI
操作表和签名未扩展。相关 C++ 消费者需使用新 SDK 重编，本轮已实际运行安装消费者，不承诺旧宿主 DLL 的布局兼容。

输入使用 strict raw table，不执行 __index/__newindex 来补值；拒绝缺失、nil、未知/non-string 键、数值字符串、
越界/非整数整数输入及未列 enum。字段按 C++ 声明顺序读取和构造，不按 Lua 遍历顺序构造。
参数失败不调用 provider；结果失败清理转换 scratch 并报告错误，但已经执行的 provider 业务不回滚。

typed 槽位无需默认构造 T；成功槽位精确拥有对象，逆序 reset。const 成员可聚合初始化，非默认构造资源用显式
no-throw 工厂。当前 Ability 输出槽本身要求可赋值，故 const 不可赋值记录可作参数/值推送，不能作为该输出槽返回。
新非平凡跨挂起结果、任意对象图、任意容器和新对象绑定均不在支持范围。

LuaJIT 与 Lua54 分别验证初始化和 pcall 错误边界；C trampoline 没有 owning C++ 局部对象，typed 对象在其外侧，
OOM 不依赖 VM 替它析构。scratch 只释放本次转换新增的普通栈槽。每 record 至多 64 字段，类型深度至多 32，
typed 存储与整个签名受 64 KiB 合计门槛限制；这是保守的 typed 存储费用，不是机器栈使用的精确承诺。
用户规则不能保留 Reader/Writer、跨挂起借用或绕开其声明的有限存储/深度契约。

## 3. 实际验证及旧断言对应

最终 6086e4a4：RelWithDebInfo，`cmake --build <build> --target all -j 4 -- -k 0`；构建与测试/性能串行。

| 验证 | 实际结果 |
|---|---|
| Toolchain 全量 CTest | 108/108，50.02 s |
| Developer 全量 CTest | 123/123，47.56 s |
| Lua54 受影响 CTest 集合 | 110/110，46.55 s；不是宣称该 profile 的所有测试 |
| 三 profile 安装与第二轮构建 | 全部通过，第二轮 `ninja: no work to do` |
| 最终安装消费者 | 原 14 个加 script-lua-values，共 15/15；各自运行及第二轮无工作 |
| 安装增量 | 13 项预期匹配；5 种输入/模板变更、64 字段成功、4 种拒绝和恢复/no-op |
| 协议轨迹与 wire | 生命周期/Event 非空轨迹、命中次数与业务计数匹配；旧 288 字节 wire golden 匹配 |

最后一项使用本轮 4fe3565e 装配探针，非伪装为 6086e4a4 的新运行。两者的核心 simulation_script DLL 哈希相同，
差异和未变产物由 final-binary-changes.json 与快照哈希证明；最终全量 CTest 则确实重新执行于 6086e4a4。
wire SHA-256：`8002ffd5678f822d79949b4d0a36d1687613216af4b26b876f967fa24cc76e52`。

| 现有入口/断言 | 本轮入口与保留/增加的实际检查 |
|---|---|
| lua_backend_test 的 CollisionEvent 参数值 | 生成 CollisionValue + makeLuaValueOperation；同一 body/impulse 值仍交到 export，不用互相 round-trip 代替业务断言 |
| collision_event_chain_test 的 native/Lua Hook→Event 链 | 同一 payload、回调/挂起/恢复计数；Event 操作在 prepare 固定 |
| typed scalar Ability 生成与实际 provider | 原 projection、Lua runtime/coroutine、Physics、CppStatic/Native/FlowForge 测试全部保留 |
| SR-2 装配与状态反馈、SR-3 七点清理重入、SR-4 Timer/移动 | 原 lifecycle/execution/event/pin/frontier/预算/安装测试保留在全量 CTest，未放宽断言或追加恢复点 |
| 新 Lua 值边界 | lua_value_test：独立 Lua literal、strict 形状、velocity.x 路径、有限 enum、const 聚合、无默认构造、逆序释放 2→1、失败后资源 live=0 |
| 新 OOM/栈/错误 | 有活资源时 table/key 分配失败、错误文本失败 false/nil、checkstack 拒绝、trampoline 初次初始化失败后恢复；资源只释放一次 |
| 新实际生成/构造 | lua_value_runtime_test：Pose 双向字段算术、const 参数、no-copy ValueToken 工厂、第二参数失败 provider=0/release 一次、工厂拒绝无资源 |
| 新方向一致性 | push-only 所需 reader 冷期 UNSUPPORTED_ABILITY_TYPE；相同类型表示冲突 INVALID_VALUE_OPERATION；同布局不同语义指纹不同 |
| 新重入权限 | 共用 VM 的两个 runtime 合法嵌套 provider；转换中真实 Entity 销毁/关闭使原 provider=0；保护内 query 为 ENDPOINT_BUSY；EndPlay 仍恰好一次 |
| 新返回/错误边界 | 非法 enum 结果 provider=1且记录失败；捕获退休错误后再调用不能重获资格；未终止 custom 错误数组有界转换 |
| 新安装生成失效 | include 字段与宏改变编译后表示指纹；annotation/rule/template 精确重生成一次；重复键、private、65 字段、union 拒绝且旧输出哈希不变 |

六项 Scene Lua 测试在本轮 Developer/Lua54 均通过，使用本轮 Toolchain 实际 portability artifact。
其契约/预期修正在 SR-4 已被接受，本轮不认领该修复，也不再把通过结果写为“历史失败未运行”。
新 CTest 含 `/UNDEBUG`，RelWithDebInfo 中 assert 业务检查实际执行。

## 4. 成本与证据读法

本轮先完成 4fe3565e 的八场景五组配对，再由实际优化产物定位固定正索引仍走 DLL absolute、成功路径构造错误缓冲。
6086e4a4 只修这两点，没有无依据 forceinline/LTO/预算变更。两轮原始数据均保留；最终八场景另跑，不给旧 CSV 改标签。

每个配对是独立进程；固定 seed、工作量、容量、VM、worker、warmup、恢复预算。汇总同时校验逐行业务量和 backlog，
不是仅比较用时。全批除以实际调用/恢复数量是摊销成本，不是隔离 resume 或单 handler 的成本。
旧 CSV 无 error/failure 字段时保持 null；Flow 的计时外 INTEGRITY 记录和新值探针的错误/业务断言单独保存。
时间诊断与分配诊断分开；计数开关关闭时原日志的 0 不能作为“零分配”证据。

| 场景 | 初轮 4fe 配对中位差距 | 最终配对中位差距及范围 | 最终 B0/B2 全批中位 ms | 每批实际操作数 | B0/B2 ns/操作 |
|---|---:|---:|---:|---:|---:|
| cpp-update-10k | -0.20% | +3.34% [-8.37, +8.41] | 224.361 / 228.149 | 50,000,000 | 4.487 / 4.563 |
| lua-update-10k | -2.30% | -3.31% [-5.58, +0.20] | 1300.514 / 1259.537 | 50,000,000 | 26.010 / 25.191 |
| lua-ability-10k | +21.11% | +14.06% [+5.81, +54.54] | 20.133 / 22.730 | 300,000 | 67.111 / 75.767 |
| lua-coroutine-10k | +0.06% | +0.77% [-1.44, +22.37] | 6838.551 / 6839.191 | 10,000,000 | 683.855 / 683.919 |
| lua-event-10k | +6.48% | -0.91% [-1.99, +44.68] | 30660.090 / 30107.356 | 50,000,000 | 613.202 / 602.147 |
| flow-update-10k | +0.65% | -1.41% [-7.82, +1.03] | 428.643 / 408.438 | 50,000,000 | 8.573 / 8.169 |
| flow-event-10k | -0.26% | +1.23% [-2.73, +7.92] | 5377.457 / 5648.783 | 10,000,000 | 537.746 / 564.878 |
| event-fanout-10k | +0.23% | -1.88% [-11.55, +1.75] | 2.233 / 2.056 | 10,000 | 223.320 / 205.570 |

B0 为上述 cd7160ff 实际参照，B2 为 6086e4a4；两者 CSV 身份和工作量经汇总脚本校验。
初轮与最终是分别相对参照的两轮配对，不是把两轮中位数之差冒充同一组直接 B1→B2 实验。
参照本身的跨轮绝对耗时也有明显漂移，尤其 Lua Event；因此跨轮绝对差不能全归因于两项代码调整。
Lua Ability 最终五组均为正差距，仍有新增且未充分解释的成本，**性能未收口，待用户审阅接受或另行授权处理**。
初轮 Lua Event +6.48% 在最终组中位数未重现，但最终有 +44.68% 的配对离群；不能据此宣称消除回退或性能等价。
其他小差距和宽分布按原样登记，五个进程对不足以证明等价，也不按每项残余开启新调优项目。
旧驱动完整保留同量工作：每批 Hook candidates 为 frames×10,000；Flow Event/Lua coroutine 每帧实际新调用和恢复为预算2,000，backlog8,000；
Lua Event 每帧实际新调用与恢复10,000、backlog0；fan-out 为一次 occurrence 完成10,000个等待者并按原预算排空。

独立两字段输出：旧手写推表 100.220 ns/次，生成受保护推表 261.197 ns/次，中位批量 10.022/26.120 ms（100,000 次），
五组配对 +150.75%～+173.11%。新增约 161 ns/次是本轮真实成本；不能凭错误保护的功能收益自动宣称它必然最优或等价。
两个 probe 的字段值、工作量与栈输出数一致。单独 Lua allocator 诊断为旧 201,654 次/11,252,928 字节，新 200,000 次/11,200,000 字节；
它不包含全引擎堆，且分配诊断的耗时不混入时间配对。

新嵌套 Pose 双向真实 Ability：100,000 次 provider，1000 warmup，中位 144.202 ms / 1442.017 ns 每次，错误=0、backlog=0。
单独 VM 分配诊断为 800,000 次。参照版本没有该双向能力，故只给绝对结果，不编造旧实现为零。

本轮 4fe3565e 的冷期配对：prepare 139.5→140.0 µs，late 11.3→11.0 µs，128 次单实例重建 102.3→104.9 µs。
三者 EXE-local 分配诊断两侧均为 5/33/535；create=destroy=152、prepare=release=456。
8/8192 配置、各 16 warmup +128 重建：候选中位 93.1/100.3 µs，总量128个配置槽访问、0个 endpoint count 访问，
错误/backlog=0；诊断没有恢复全目录复制。8配置第5组有 +128.07% 的时间离群，原样保留，未删样本。

历史债务单列：此前 H0→SR4 持续 FlowForge Event 约 +11.14%；Lua Update +1.52%（约0.3912 ns）、prepare +7.61%
（约10 µs）、执行/来源索引每实例 +24 字节仍参见既有报告。这些不是本轮候选的新成本免责，也未被重标为必要安全成本。

## 5. 无效试验、交付与停止

保留中间生成/编译失败：重复预包含、MSVC dependent requires/constexpr ICE、const 非可赋值结果槽、类型名/行宽门禁。
测试夹具曾因不同资产数超出其 prepared catalog 容量而失败；修正有限夹具容量，未扩大 runtime 恢复预算。
保护窗口内 query 的既有返回是 ENDPOINT_BUSY，修正新测试对该返回的错误预期，未修改生产权限。
旧规模探针只认识 `main()` 而当前 fixture 为 `main(argc,argv)`，首轮未运行；修正精确一次命中适配后重放。
独立输出成本探针漏配 compile_commands.json 的构建失败也保留，未执行旧二进制，修正驱动后才形成有效配对。

原始记录在 [SR-5 仓库证据索引](evidence/script/sr5/final/README.md)：ZIP 16,341,809 字节，1,763 项文件，
SHA-256 `8cb121fdeebb157aaadcff09c688a32850df17285759fdc8b0cb0f7ea3313438`。
归档内逐文件哈希已复核；没有编译二进制。索引提供实际源提交、安装头/生成器/DLL 哈希、执行参数、退出码、
原始 CSV/日志/生成结果和完整下载入口。远端独立 fetch 后重新读取归档与索引，全部 1,763 项文件哈希复核通过，
校验记录已随索引交付。

本轮没有承诺任意类型支持、无额外成本或性能等价；支持矩阵外的类型/跨挂起协议继续拒绝。
新增成本和有限统计结论提交用户决定，停止等待审阅，不自动转入 SR-6 或继续全面调优。
