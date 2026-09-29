# P00 — 基线盘点、反向依赖门禁与迁移账本

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** 无；本阶段核验仓库与基线。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** 全局边界；尚不建立新业务 owner。**关联问题：** A01—A08、B01—B04。**V3 回归：** Q31, Q38, Q42, Q45, Q49。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `architecture.json（数据，不是 C++ 类型）` | `docs/editor-redesign/architecture.json` | 构建边界唯一清单 | 逐 target 记录逻辑名、真实 CMake target、源码/公开头目录、直接依赖、禁边与首次实施阶段。 |
| `migration-ledger.json` | `docs/editor-redesign/migration-ledger.json` | 迁移管理记录，不进入产品 | 每个旧符号/文件有事实证据、引用点、去向、责任阶段、删除期限、当前状态和实际实现 SHA。 |
| `test-coverage.json / baseline.json` | `docs/editor-redesign/test-coverage.json` | 证据记录 | 保留全部既有测试的语义与替代去向；未执行、失败、通过分别记录。 |
| `阶段收据模式` | `docs/editor-redesign/receipts/schema.json` | 交付契约 | 禁止用测试数量替代功能覆盖；implementation_sha 与收据提交分离。 |
| `EditorArchitectureChecks.cmake` | `cmake/EditorArchitectureChecks.cmake` | 独立构建检查 | 按实际 target 属性检查直接/传递边与生成工具边；不修改被测代码使错误消失。 |
| `check_editor_boundaries.py` | `editor/tests/architecture/check_editor_boundaries.py` | 测试工具 | 检查 include 路径和编译数据库；不能把正则结果宣传成完整 C++ 语义分析。 |

## C. 顺序与范围

先盘点，再形成机器可检查的边界，最后运行基线。禁止本阶段将旧目录整体移动或批量改名。当前 main 的参考提交为 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`；若实际工作分支有后续提交，不 reset 回这个版本。记录 `baseline_sha`、当前分支、未提交文件，并用 diff 核对本实施包涉及的旧符号是否变化。

本阶段的输出不是又一份设计散文，而是后续每次删除能追踪的实际清单。覆盖范围必须包含 `editor/`、其依赖的 UI/scene runtime，以及引用 editor 的 `examples/`、插件、安装消费者、生成脚本、CMake、测试。FlowForge、Project/资源浏览器、TaskPane、SceneCreationPane、Inspector codegen、launcher 都在盘点范围，不能仅扫描 Scene/Material。

### C0. 将规范与可变交接记录落到仓库

先将本包原件完整放入 `docs/editor-redesign/spec-v4/`（原有同名目录存在时先比较，禁止覆盖用户修改），使每位后续 LLM 可以读取同一阶段说明。此目录是固定规范与证据种子，不是产品源码。把 `manifests/architecture.json` 复制为 `docs/editor-redesign/architecture.json`，把 migration-ledger.seed.json/test-coverage.seed.json 复制为同目录无 seed 的文件，补真实 HEAD、字段引用和执行结果；不得修改种子让它假装本来就已验证。

新建 `baseline.json` 记录构建环境/实际基线/功能矩阵，建立 `receipts/`，将随包 receipt.schema.json 作为模式，按 receipt.template.json 填真实收据。P00 没有前驱，所以 predecessor_receipts 为空；后续阶段必须记录实际前驱实现 SHA。完整规范的 scripts 只作辅助扫描，实际架构检查实现仍进入本阶段规定的 tests/architecture 和 CMake。

### C1. 先完成可重复命令

```sh
git status --short
git rev-parse HEAD
git submodule status --recursive
git ls-files editor modules engine cmake examples .github
cmake -S . -B build/editor-baseline -DLUX_BUILD_PROFILE=EDITOR -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/editor-baseline --config RelWithDebInfo
ctest --test-dir build/editor-baseline -C RelWithDebInfo --output-on-failure
```

上述 profile/标准来自当前根 CMake；第三方安装前缀沿用实际环境，不写死任何人的 D 盘目录。若工具链或依赖缺失，记录准确缺项；不得下载安装未经请求的工具、不杀用户进程、不清除工作树。基线失败是证据，不可伪造成功，但可在已定位的构建配置修正后重测。

### C2. 建立可穷举清单

1. 用 `git ls-files` 固定全部被跟踪文件。枚举头中的 class/struct/enum/alias，以及成员/函数的声明、定义和使用点；优先复用已有 Clang 工具链与 `compile_commands.json` 提取 USR/限定名。若某些生成配置无法 AST 解析，保留文本候选并人工核对，状态不得标 `verified_ast`。
2. 对 `Editor::Impl`、`SceneEditor::Impl`、`MaterialEditor::Impl`、`FlowForgeEditor::Impl`、`EditorContext`、`PaneManager`、`EditHistory` 分别输出字段与成员函数的完整表。每一项都标记为“领域/视图/运行/保存/用例/通知/派生/纯机制/删除”。未分类项数量必须为零。
3. 搜索每个旧接口的真实消费者，特别是 `beginSave/finishSave`、`HistorySnapshot::clean`、Root 构造挂载、`valid/invalid`、`setCommands`、`code_lifetime`、`sinclude` 和旧 target 名。字符串数据和 C++ 类型引用分开。
4. 盘点公开安装头、导出 target、运行插件 SDK 输入和编辑器扩展输入。识别 `cmake/installed-consumers/editor-d2` 中测试支撑与安装消费的混用；记录哪些现有测试应移到哪一层。
5. 保存原资产、旧布局与插件样本的哈希。只使用副本进行测试，不能让基线测试覆盖用户真实项目。

每行账本至少有 `legacy_path / qualified_symbol / kind / references / destination_module / replacement / first_action / removal_deadline / disposition / proof`。初始本实施包的已核对清单是种子，不代表仓库所有符号已经被本次静态读取穷举；本阶段必须补全实际引用，不能靠“文档没写这个类”保留孤岛。

## D. 构建门禁在本阶段落地

建立最终依赖白名单，但只检查已经存在/本阶段已引入的 target；不能创建 39 个空库冒充边界完成。直接边、公开头、生成依赖分别记录。源树中 `scene_model → Pane`、`editor_workflows → editor_bootstrap`、`engine → editor` 应通过专门负向夹具导致失败。

将测试分为 native、desktop、GPU、toolchain、installed。建议新增并实际实现以下选项：`LUX_EDITOR_BUILD_NATIVE_TESTS`、`LUX_EDITOR_BUILD_DESKTOP_TESTS`、`LUX_EDITOR_BUILD_GPU_TESTS`、`LUX_EDITOR_BUILD_TOOLCHAIN_TESTS`、`LUX_EDITOR_BUILD_INSTALLED_TESTS`，均受 `BUILD_TESTING` 总开关约束。native 默认开启；其他组按实际条件显式选择。它们是新配置，不冒充旧版本已有。

从所有普通测试的配置路径移走强制 `find_program(lld-link REQUIRED)`、强制 PowerShell 和本机路径 hint；仅真正链接 FlowForge 产物或平台集成的测试需要相应工具。缺少 lld-link 不能禁用 FlowForge 作者模型/图编辑测试。测试分组不能从产品构建中删除 FlowForge 功能。

## E. 保留可验证的失败，避免以重构为名降低门槛

优先给 C01（坏 docking）、C03（QUERY 自替换）、C04（创建时菜单失败）增加固定输入和故障注入点，记录旧路径行为。可使用独立缺陷复现工具；旧实现确有失败时记录 `KNOWN_BASELINE_FAILURE`，不要写一个反向断言把错误行为定义成未来正确行为。

现有测试的每个重要断言必须映射到新 Q/X 场景。允许迁移测试而不是维持旧私有 TestAccess，但不能通过删除旧测试获得更少的失败。P00 定义的基线例外只适用于被记录的旧路径，在新路径对应阶段必须消除；不能 blanket xfail。

## F. 阶段出口

基线命令、target 实际映射、完整旧符号账本、已有功能覆盖表和三类缺陷的证据齐全。可以有已记录的旧缺陷，但依赖门禁和测试分组选项必须真实运行。P00 不要求在所有 GPU/IME 硬件上先完成整个最终资格；最终不可省略的环境矩阵在这里记录，并在对应阶段执行。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| editor/app/CMakeLists.txt 中普通测试对本机 lld-link/PowerShell 的无条件要求 | 移动到确实需要的 toolchain/平台测试组；删除 D 盘特定 hint | 分层测试配置；P00 | 真实 FlowForge 编译/链接及 Windows 集成测试仍保留 |
| 未跟踪的重构临时文件、用户本地资源和旧布局 | 不删除；先记录来源和用途 | 用户工作区保护；永久保护 | 用户未提交改动、测试负证据、真实数据不属于垃圾代码 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X00-01 / `boundary_negative_fixture` | 故意让 scene_model 样例 include Pane | 检查器因禁止的依赖失败；移除错误边后同环境通过 |
| X00-02 / `native_tests_without_platform_linker` | 无 PowerShell/lld-link 的 native-only 配置 | 领域测试可配置；真实 linker 测试显式未选择，不是整个工具功能被删 |
| X00-03 / `legacy_inventory_is_total` | 遍历四个 Impl、Context、PaneManager 的声明与引用 | 每项都有处置与期限；未知项不为零则不能进入 P01 |
| X00-04 / `baseline_failures_are_not_successes` | 读取失败日志和既有测试清单 | 原失败保留；测试迁移覆盖未因删测试而缩减 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P00.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
