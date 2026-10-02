# P12 — 产品用例闭合、唯一入口切换与旧框架删除

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P02, P03, P04, P05, P06, P07, P08, P09, P10, P11。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M15 / M16；全产品唯一新路径。**关联问题：** A01—A08、T01—T09、C01—C04。**V3 回归：** Q03, Q04, Q05, Q15, Q18, Q19, Q20, Q21, Q31, Q33, Q36, Q37, Q40, Q41, Q42, Q43, Q51。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `OpenAssetOperation / OpenId / OpenResult` | `editor/workflows/include/lux/engine/editor/workflows/OpenAsset.hpp` | 具体操作集合拥有，不是万能 workflow 基类 | session publication 与 presentation 结果分开；相同资产默认复用工作副本。 |
| `ReloadSessionOperation / ReloadOutcome` | `editor/workflows/include/lux/engine/editor/workflows/ReloadSession.hpp` | 候选与原子采用用例 | 处理在途写入、审阅戳；保留 SessionId，成功更新 HistoryId，失败不 clear。 |
| `CloseViewOperation / CloseSessionsOperation / CloseViewPolicy / CloseReport` | `editor/workflows/include/lux/engine/editor/workflows/CloseSessions.hpp` | 不同生命周期动作 | 固定被审阅内容集合与 stamp；全部许可后才提交删除；Cancel 不回滚已保存。 |
| `SaveAllOperation / SaveAllReport` | `editor/workflows/include/lux/engine/editor/workflows/SaveAll.hpp` | 固定会话集合的保存编排 | 按 Session 而非 Pane 保存；逐项记录准入/结果/缺角色，不能重复保存两视图同源。 |
| `ApplyLayoutOperation / LayoutApplyReport / RestoreSessionOperation` | `editor/workflows/include/lux/engine/editor/workflows/ApplyLayout.hpp` | 布局结构提交与内容恢复分开 | pure plan→prepare→safe commit→report；偏好保存独立结果。 |
| `InstalledSession / SessionViewBindings / SessionCloseDependencies` | `editor/workflows/src/SessionInstallation.hpp` | 角色注册与关联记录，不拥有第二份源 | 撤销角色/视图/运行依赖后 Store 消费许可；领域退休 hook 由 integration 注入。 |
| `ModelCreationOperation` | `editor/tools/scene/integration/src/ModelCreationOperation.hpp` | 领域特定长期操作 | IO产物校验后一次 SceneEditBatch；载入完成不等于模型插入完成。 |
| `EditorApplication / EditorApplicationConfig` | `editor/application/include/lux/engine/editor/EditorApplication.hpp` | 生命周期装配根 | create 返回完整成功对象；exec 驱动既有宿主；不保存各用例中间字段。 |
| `工具/桌面/用例装配函数` | `editor/application/src/ 与 editor/tools/*/integration/` | 连接具体依赖，不作业务参数 | 唯一产品 lux_editor 指向 editor_bootstrap；没有 old/new 路由开关。 |

## C. 本阶段是原子切换门槛，不是“接一半再留后门”

先使完整新功能链通过集成测试，再在同一阶段把唯一 launcher/产品 target、内置插件、菜单、例子和安装入口切到新结构，随后删除本阶段账本列出的旧框架。不能只在测试程序演示新系统、正式产品仍走旧 EditorContext，然后把阶段标为完成。

允许阶段内部多个提交，但最终收据必须指向已经切换且已删除旧路径的可构建提交。P13 不是留给旧 Context/SceneEditor 大类的延期；若它们仍被一个真实消费者需要，本阶段 BLOCKED，定位该消费者并完成迁移。

## D. 每种用例的准确状态、提交点和失败事实

### D1. OpenAssetOperation

顺序：校验 AssetAddress/工具 kind → 查相同工作副本策略 → 固定扩展 snapshot → 读入/解码 → owner 预留 SessionId → 构造候选及角色 → 一次发布 Session 与配套角色 → 复用/创建视图 → adopt/show/focus → 结果。

OpenResult 必须能表达：全部失败；成功复用/新建 Session 且显示成功；Session 已成功发布但视图创建/显示失败。最后一种保留可访问的无视图 Session，不能给用户一条“打开失败”而隐藏已发生事实，也不能未经决定删除成功内容。打开第二个视图不触发第二次工作副本读取。

LoadJob 只保留地址/冻结输入/代码 owner；使用同一操作 ID 处理迟到结果，取消或代际变化的输出不采用。新的会话 reserve 尚未发布时其他 lookup 不可见。

### D2. ReloadSessionOperation

先处理该 Session 的未保存审阅和在途写入。普通保存仍可能向同一文件发布时，必须先得到其准确终态或明确拒绝重载，不能读到新内容后又被旧保存覆盖。记录被审阅 ContentStamp；读入期间作者如果继续编辑，采用前发现变化要重新审阅或返回冲突，不丢弃未经审阅内容。

候选把新源、索引、新 HistoryId/StateId、checkpoint 与角色更新一起准备；成功保留 SessionId 一次交换，旧 history/source 在允许阶段退休。失败保留全部原内容/历史/绑定；禁止 `clearAsset(); load()`。旧 compile/save/projection 结果由于旧 HistoryId 被拒绝或重建。

### D3. CloseView / HideView / CloseSession / Exit

HideView 只改显示；CloseView 销毁一个视图；CloseSession 审阅并销毁内容；Exit 审阅固定的一组内容后退出。四者不是 CloseRequest 的四个任意 action 分支。

CloseView 的最后视图策略显式为 AskKeepSession：用户可只关视图保留内容、同时关闭内容、取消。选择取消之前不先销毁最后视图。关闭普通视图不停止独立 Run，不删除源；关联运行的策略在关闭内容时明确询问或停止，保持运行时必须证明只依赖冻结输入。

CloseSessionsOperation 的顺序：固定 Session 集 → 对每份内容收集含 stamp 的用户选择 → 所需保存完成并校验当前是否仍 dirty → 准备相关运行/任务/视图退休 → 取得全部 ClosePermit → 准备解绑批次 → 在允许阶段提交删除 → 报告。

A 已保存、B 后来 Cancel：不得提前销毁 A；A 的磁盘保存保留，所有 permit 释放。对 S10 的 Discard 回答到来时若已变 S12，不得直接丢弃 S12。拿第二张 permit 失败时释放第一张，集合仍完整。

SessionCloseDependencies 是用例层受限退休协议，返回明确的 pending/completed/rejected 与拥有的准备资源；具体 RunId/GPU/编译细节由工具 integration 的 hook 实现，workflows 不 include RunStore/Scene 私有头。它不是 getService，也不能调任意析构去猜是否完成。提交后的异步退休依赖独立 pin/快照，不再借已删除源。

### D4. SaveAllOperation

固定启动时的 SessionId 集合，按会话去重，不以 PaneId 枚举。每份可保存内容独立 requestSave；无绑定需要 Save As 的项、无保存角色的项、用户取消与真实失败分别报告。后续新开的 Session 不悄悄加入本次 Save All。只返回 requests accepted 不能显示“全部已保存”。

从 UI 明确 Save 时，先按交互策略结束所绑定组的编辑；提交 gesture 失败则不准入保存。SaveService 自身不引用 UI，它只保存已提交快照；程序化保存不会暗中提交别的视图的手势。

### D5. ApplyLayout 与 RestoreSession

Parse/Validate → 固定扩展/视图快照 → 纯 Resolve → prepare detached/state/mount batch → 处理受影响交互 → commit structure → report → 可选偏好保存。默认 CancelPreview 清理布局涉及的未提交手势，不隐式产生作者编辑；不销毁额外视图或替换 dirty 内容。

缺插件/未知 schema 按占位或遗漏策略报告并保留 payload。provider 的构造/restore 不准访问网络、保存文件或改 Session；这不是对任意插件副作用做通用事务。结构 commit 不再调可失败 provider 方法，通知中的结构请求下一批处理。

RestoreSessionOperation 读取独立 RecoveryManifest，再通过 OpenAssetOperation 恢复内容/绑定。它不承诺恢复未保存修改；只有另有真实恢复快照时才可声明。不得又把恢复 locator 塞回 DockLayout 然后自动覆盖用户当前内容。

### D6. 模型插入、编译与发布请求

ModelCreationOperation 持模型读取任务、ContentStamp、插入参数和终态；解码成功后验证目标会话/历史与编辑许可，再一次调用 SceneSession 的批量插入。失败/取消清理候选但不污染源；完成 ID 表示实际插入，不只是文件读完。

材质/Flow 编译、链接重试和产物发布入口接到 P07 对应服务；不通过作者保存接口伪装产物写入，也不把编译成功当成源码已保存。先前已有功能按 baseline feature matrix 逐项接通。

## E. 应用根的创建、驱动与关闭

`EditorApplication::create` 先构建实际依赖、注册/工具、菜单、布局存储/默认准备等必要组件；任一步失败返回错误且不发布 application。旧 initializeMenu(void) 内 fail 后 create 仍成功的控制流必须彻底消失。运行期 fault channel 与构造 Result 分开。

EditorApplication 只拥有服务、调度相位和应用级 lifecycle 状态，不拥有 menu target 的裸 Pane*、各 save/candidate/read_result/close_decisions 字段。具体 records 各属于自己的用例服务；它们不共同修改一个 SharedApplicationState。

参考驱动：收输入与明确请求 → owner 完成采用 → 推进具体用例 → engine 单一 driveFrame → 投影/呈现准备 → UI 安全结构提交与通知 → 绘制/提交/退休。真正的相位依赖在接口中验证，不让每个组件靠“碰巧在前面 tick”维持安全。回调中的结构变化排下个允许阶段。

退出顺序：停止新业务准入 → 完成/取消并明确终态 → 排空 owner 完成队列 → 停止/退休运行和预览 → 卸载视图并完成 GPU 退休 → 撤销角色后销毁 Sessions/history/source → 销毁仍需插件代码的结果/任务 → 释放扩展 lease/插件 → 平台/执行器。不得停止消息泵后又等待只能由此泵完成的工作；析构不弹对话框、不偷偷保存、不创建新任务。

## F. 旧框架逐项删除清单与切换证据

精确成员表在附录账本；本阶段必须使以下旧 C++ 类型、函数和 active target 不再存在：

- `EditorContext`、`PaneManager`、旧 `Editor::Impl`/旧 Editor 创建入口；`EditorContext::engine/execution/project/panes/setCommands/openAsset` 全部调用迁走。
- `SceneEditor`、`MaterialEditor`、`FlowForgeEditor` 与其 Impl、UI friend、旧 TestAccess；旧 Asset 开关/Close/保存方法和内部 pending 状态一并移除。
- `WorkspaceRequest`、`EWorkspaceAction`、`WorkspaceData`、业务 `CloseRequest/CloseDecision/ECloseDecision/EClosePurpose`、`AssetEditStatus/EAssetChange/EAssetChangeDecision`。
- 旧 `PaneRegistration/AssetEditorRegistration/CommandRegistration` 及其旧 create/restore/invoke；旧注册元信息生成输入、旧 DLL导出/安装 target 和 CMake PUBLIC 传播。
- `TAssetSave` 与 SceneSave/MaterialSave/FlowSave、SceneSaveCapture::copied、所有过渡 `LegacyPersistenceState`/桥、旧 rooted constructor / root() 假设的 compatibility入口。
- 老 `editor_context/editor_ui/editor_scene/editor_material/editor_flowforge` 等总 target 的实际源定义和别名在完成重归属后删除；不得移成 INTERFACE 链接全部新库来保住旧名字。

必须保留：纯 EditHistory/EditOperation、低层资源/执行器/场景/UI安全机制、既有资产编码算法、数据只读迁移、旧布局备份、实际有用的负向测试样本。`EditorError.hpp` 中若仍有真实通用错误事实，迁到其正确归属后删旧混合入口；不能把诊断全删来满足关键词检查。

不得整目录 `rm -rf editor/tools/scene`，因为里面现在已经有新模块。逐文件处置，并同时更新所有例子、插件、生成输入、安装目录与活动测试。参考子目录旧 src/pinclude/include 为空后删除；源代码备份用 Git 历史，不在树里留 `.old/.bak/disabled/legacy2`。

阶段出口要求唯一 `lux_editor` 来自 editor_bootstrap；不存在能运行旧架构的用户开关、安装文件、旧 DLL alias 或测试专用回落。新的完整用例均跑通，旧框架审计零 active 引用后才可 PASS。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| EditorContext.hpp/.cpp；PaneManager.hpp/.cpp；editor/context 旧构建入口 | 迁走所有实际消费者后删除类型/文件/导出/别名 | 窄服务 + SessionStore/ViewHost/extensions/workflows；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| EditorImpl.hpp、Editor.cpp/EditorStartup.cpp/EditorMenu.cpp/EditorWorkspace*.cpp 与旧 Editor API | 根改为 EditorApplication；用例/菜单/工作区分属各模块，删除旧文件 | bootstrap/commands/workflows/workspace/desktop；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| SceneEditor/SceneEditorImpl；MaterialEditor/MaterialEditorImpl；FlowForgeEditor/FlowForgeEditorImpl | 迁走剩余 UI/资产/关闭/保存/运行职责，删除旧类及 friends/TestAccess | 对应新 model/persistence/execution/preview/ui/integration；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| WorkspaceRequest/EWorkspaceAction/WorkspaceData；CloseRequest/CloseDecision/ECloseDecision/EClosePurpose | 删除混合业务协议及原定义文件 | 纯布局值/具体用例/ClosePermit/结构化结果；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| AssetEditStatus/EAssetChange/EAssetChangeDecision；AssetEditorRegistration；旧 PaneRegistration/CommandRegistration | 删除原业务类型和已迁注册文件，不保留兼容 class alias | Open/Reload 用例与 typed extension API；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| TAssetSave/SceneSave/MaterialSave/FlowSave/SceneSaveCapture/旧 SaveRequest 业务入口 | 删除所有声明/实现/注册/活动测试调用 | SaveService + 实际新 SaveId/Outcome；不同名旧 ID 不继续传播；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| editor/transition/；旧 rooted Pane/Element 兼容入口及 LegacyPersistence* | 全部删除；不得延期到 P13 | 只有新 detached 构造与新 owner；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧总 target/安装包别名/生成入口/新旧运行选择开关 | 更新唯一产品后删除，不建立聚合兼容壳 | 唯一 editor_bootstrap → lux_editor；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X12-01 / `open_session_success_view_failure` | Session 发布后注入视图构造失败 | 保留无视图内容，OpenResult 表达真实部分完成 |
| X12-02 / `reload_waits_for_old_publish_and_checks_stamp` | 旧保存仍发布、重载读入期间内容变化 | 无晚写覆盖新事实；未经审阅的新编辑不丢失 |
| X12-03 / `close_all_late_cancel_destroys_nothing` | A 保存成功、B 取消；另测第二张许可失败 | 所有 Session 仍在；A 真保存保留；许可释放 |
| X12-04 / `close_last_view_three_choices` | 最后视图选择 keep/close/cancel | 三种行为准确，取消前视图不被销毁 |
| X12-05 / `layout_non_destructive_full_pipeline` | dirty 双视图+额外窗口+缺插件+偏好写失败 | 内容/历史完整；报告实际结构与独立偏好失败 |
| X12-06 / `save_all_deduplicates_sessions` | 两视图同一源、一个无绑定源、运行中新开会话 | 按启动会话集合去重，逐项完整结果，无假全部完成 |
| X12-07 / `model_load_is_not_insertion_success` | 模型读取成功后源已换 HistoryId/插入预算失败 | 最终失败而非已插入；源未半改 |
| X12-08 / `startup_menu_failure_prevents_publish` | 各构造阶段/菜单注册连接失败 | create 返回错误，无成功但已经 fail 的对象 |
| X12-09 / `exit_drains_before_destroying_dispatcher` | 编码/运行/GPU/插件回调同时在途 | 无死锁/UAF/代码早卸载；真实退休责任结清 |
| X12-10 / `only_new_product_path_and_zero_legacy_owner` | 扫描所有构建入口、安装、AST和运行 target | 唯一新产品；所有限期桥与旧大类删除，零永久回落 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P12.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
