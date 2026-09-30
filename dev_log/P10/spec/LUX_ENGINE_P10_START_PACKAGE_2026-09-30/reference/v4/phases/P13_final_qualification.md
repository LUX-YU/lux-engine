# P13 — 工程收尾、独立安装、全量资格与交付封存

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P12。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M16；所有责任域的最终证据。**关联问题：** B01—B04；全部已修问题的系统回归。**V3 回归：** Q01, Q02, Q03, Q04, Q05, Q06, Q07, Q08, Q09, Q10, Q11, Q12, Q13, Q14, Q15, Q16, Q17, Q18, Q19, Q20, Q21, Q22, Q23, Q24, Q25, Q26, Q27, Q28, Q29, Q30, Q31, Q32, Q33, Q34, Q35, Q36, Q37, Q38, Q39, Q40, Q41, Q42, Q43, Q44, Q45, Q46, Q47, Q48, Q49, Q50, Q51, Q52。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `最终 architecture/retirement/test evidence` | `docs/editor-redesign/` | 证据，不新增产品类型 | 账本中全部过渡已关闭；最终模块、公开头、安装和功能覆盖一致。 |
| `分层 CI workflow` | ` .github/workflows/ 下对应 Editor/Player 工作流` | 工程门禁 | 每次相关 PR 跑 native/边界；desktop/GPU/toolchain/installed 按真实环境运行并保留结果。 |
| `独立消费者与公共头测试` | `editor/tests/installed/ 与 editor/tests/architecture/` | 测试支撑不进产品库 | 安装后 clean prefix 构建，不读取源码/构建私有头；负向依赖夹具仍有效。 |
| `性能/容量基准` | `editor/tests/benchmarks/` | 可复现实验，不作为新框架 | 测启动/交互/快照/多视图/GPU 退休/长期任务保留，不预设提速。 |

## C. 不准以收尾阶段掩盖未切换的架构

P12 的旧类型/入口/桥删除已是进入条件。本阶段如果仍发现 EditorContext、SceneEditor owner、旧 Pane 工厂或 transition 代码，回退阶段状态为 BLOCKED 并补正 P12，不把“以后再删”写成 P13 通过。

不再新增业务框架或替换稳定库。只处理确切的安装导出、文件残留、构建边界、测试支持、文档一致性和实测发现的缺陷；功能修复必须关联原阶段并重新执行受影响门槛。

## D. 全工程清理清单

逐项检查 `git ls-files`、所有 add_subdirectory/target_sources、生成器 TARGET_FILES、导出宏、组件包/alias、find_package 路径、安装 header 清单、例子、插件 sdk inputs、脚本硬编码、测试数据引用。

删除空目录、无引用 private header、仅转发旧类的新壳、废弃 CMake 分支、旧 selector、没消费的 helper API、未使用的 #include、for-test-only 的产品写后门与空 stub。日志/审计文档保留在 docs；失败数据/旧格式 fixture 保留在 tests/data；用户文件备份在数据目录，不移到源码 pretending code。

metadata/sinclude/pinclude 不是全仓库禁词；真正剩余的受限内部边界有 owner、target 和理由即可。禁止的是无边界地从 sibling 内部路径取头；不能为了关键词零命中把正常生成/private 机制全部删掉。

测试支撑单独 target。旧 SceneEditorTestAccess/MaterialEditorTestAccess/FlowForgeEditorTestAccess 不留在产品源中；必要注入点用具体副作用接口与 test-only fixtures，实现头不进 SDK。已有 meaningful 的故障测试全部有新 API 替代并实际运行。

## E. 构建与安装资格

执行 clean checkout（不破坏工作仓库）、新 build tree、Windows 与 Linux 配置/构建/测试；使用项目声明支持的工具链，普通 native 不要求特定 linker/PowerShell。Player 在完全不配置 editor 时构建；所有新公开头独立编译。

至少两类独立安装消费者：最小 Scene/Material/Flow 作者域消费者，以及外部 editor 扩展（工厂、命令、typed角色、代码寿命）。只提供 install prefix，环境不得泄漏源码/build path；动态库存在不等于导出闭包完整。记录 CMake 文件 API/实际编译命令中的最终依赖，不单看 PUBLIC/PRIVATE 字样。

SDK 测试包括旧 editor ABI 拒绝、新 editor 插件成功、独立 runtime 插件不受 editor-only 签名变化影响。持久载荷不支持 schema 时保留/拒绝策略准确；runtime 和 UI 功能不得因配置选项“默认关掉”绕过资格。

## F. 全部行为、真实平台与性能

复跑 V3 Q01—Q52 与本包 X 场景。Q25 若未引入 ApplyRunChanges，验收是入口明确不可用且停止不回写；不是强制新增未存在功能。已存在能力必须迁移，不能改称不在范围。所有测试的适用性在 P00冻结，最终不准临时扩大 N/A。

真实桌面/GPU 测试包括：双视图不同尺寸/相机/选择，作者+运行同时呈现，资源背压与飞行中关闭，输入捕获/焦点/IME，窗口/布局恢复、全体退出、部分启动失败。ASan/UBSan 等适用 sanitizer、GPU validation、真实文件测试各自承担不同证明，不互相替代。

容量/性能使用同机器、同构建配置、同内容和交互脚本，对比固定 baseline：启动最长阻塞、单/双/多视图每帧时间/分配、作者源数量、快照峰值/复制量、编码并发、投影滞后、in-flight GPU资源、终态记录长期增长。发现回退要分析并修正或明确阻塞，不预设“模块更多但肯定更快”。不把 debug/sanitizer 与 release 混比，不以平均值遮住卡顿。

## F2. 最终收据与发布判据

保存实现 SHA、测试环境与命令、每项真实结果、原始日志/hash、旧功能覆盖映射、零过渡账本和 install consumer 证据。文档中删除已失效的实施“暂留”建议或注明其历史状态，但保留审计历史，不能篡改过去失败。

总体完成仅在必须矩阵全部通过、旧框架零 active 路径、无未登记补丁/分支、无未知删除项时成立。某平台/GPU/IME未实际运行即明确写未验收，状态 PARTIAL/BLOCKED，不把未来会测写成已经完成。不要自行合并用户主分支、删除远程分支或发布 release，除非另有明确授权。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| 空转发壳、旧 alias、旧安装配置、无引用 include/helper、失效生成文件列表 | 按实际引用删除，补正确公开依赖 | 新 target/export 与准确文档；P13 | 已验证算法、资产兼容读取与行为回归不删 |
| 测试实现编入产品库、源码私有路径被安装消费者引用 | 迁到独立 support；消费者只用已安装 API | tests/support 与完整 SDK 闭包；P13 | 已验证算法、资产兼容读取与行为回归不删 |
| 任何 P12 到期的旧 owner/transition/双架构选择开关 | 本阶段不允许延期；发现即回报 P12 未通过 | 修正并重验 P12；P12（进入条件） | 已验证算法、资产兼容读取与行为回归不删 |
| 资产/布局兼容读取、负测试 fixture、真实失败日志与用户备份 | 保留并标明用途，不作为垃圾删除 | 数据兼容与证据链；保留 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X13-01 / `clean_install_consumer_matrix` | 隔离源树/build tree 后构建三类 domain 与扩展消费者 | 所有公开依赖可解析，无私有头泄漏 |
| X13-02 / `final_architecture_and_retirement_audit` | AST/编译图/运行入口/安装包交叉检查 | 零 active 旧 owner/桥；没有 alias/宏/注释存档式替代删除 |
| X13-03 / `real_platform_gpu_and_ime_matrix` | 执行已冻结 Windows/Linux/desktop/GPU/toolchain 组 | 每项有真实状态和日志，未测不冒充通过 |
| X13-04 / `long_running_capacity_and_latency` | 持续保存、多视图开关、编译失败/重试、GPU 背压 | 所有集合有界且无寿命错误，统计与基线可比 |
| X13-05 / `feature_and_failure_coverage_not_reduced` | 逐条对照 P00 旧功能及所有 Q/X 测试 | 无删除功能/删断言/扩大N/A换取通过 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P13.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
