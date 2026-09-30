# P01 — 历史纯化、身份、保存基线与会话唯一所有权

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P00。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M01 / M02 / M03。**关联问题：** A02、A03、A05、T05、T06、T09。**V3 回归：** Q06, Q07, Q08, Q09, Q10, Q45。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `CodeLease（复用既有代码保活机制）` | `editor/contracts/include/lux/engine/editor/contracts/CodeLease.hpp` | 小值组合，不依赖 extensions host | 包装并命名已有生命周期责任；实际对象、回调及 deleter 析构结束后才释放代码。 |
| `HistoryId / StateId / Revision / EditOperation / PreparedEdit / EditHistory` | `editor/history/include/lux/engine/editor/editing/{EditTypes,EditOperation,EditHistory}.hpp` | 保留历史协议继承和原算法 | execute/undo/redo/clear/close 保留；移出保存票据、保存中与 clean 状态；不依赖 sessions。 |
| `SessionId / SessionKey<T> / SessionKindId` | `editor/sessions/include/lux/engine/editor/sessions/SessionId.hpp` | 身份值；Store 制造 typed key | 代际与类型检查；不是 AssetId、HistoryId；不把 RTTI hash 序列化。 |
| `ContentStamp / ObservationVersion / BindingRevision` | `editor/sessions/include/lux/engine/editor/sessions/ContentStamp.hpp` | 不同事实类别 | ContentStamp=SessionId+StateId；不另造重复 SourceEpoch；通知版本不能决定 dirty。 |
| `SourceBinding / PersistenceCheckpoint / PersistedState` | `editor/sessions/include/lux/engine/editor/sessions/PersistenceCheckpoint.hpp` | SessionState 值组合 | 来源未命名/已绑定互斥；基线与持久顺序只有一个 owner；不依赖 IArtifactStore。 |
| `SessionState / EditGate / EditScope` | `editor/sessions/include/lux/engine/editor/sessions/SessionState.hpp` | 具体会话组合；scope 局部且不可复制/移动 | 管理身份、绑定、编辑准入；不保存窗口、编译、布局状态。 |
| `ClosePermit / BindingChangePermit` | `editor/sessions/include/lux/engine/editor/sessions/SessionPermits.hpp` | move-only 跨阶段许可 | 未消费析构只解除限制；已消费不再解锁被复用槽位。 |
| `IEditSession / SessionInfo` | `editor/sessions/include/lux/engine/editor/sessions/IEditSession.hpp` | 窄接口继承的边界 | describe；private prepareClose 给 Store；无 save/play/compile/Pane。 |
| `SessionStore / SessionAccess<T> / SessionReservation` | `editor/sessions/include/lux/engine/editor/sessions/SessionStore.hpp` | Store 唯一拥有 unique_ptr<IEditSession> | reserve→prepare→publish；代际查找；close permit 消费删除；不提供任意 erase 给 UI。 |
| `LegacyPersistenceState / LegacySaveTicket / LegacyPersistenceOutcome（限期桥）` | `editor/transition/LegacyPersistenceState.hpp` | 旧产品私有适配；不进新 SDK | 替代旧历史保存 API，使旧工具仍可编译；只维护本工作副本唯一 checkpoint，P12 删除。 |

## C. 先拆保存事实，再移动历史目录

`EditHistory` 已有真正的 prepare/commit/publish 与预算算法。本阶段不是再写一套通用撤销系统，而是保留这些算法、测试和命名，迁移不属于历史的保存责任。目标目录改变，`lux::editor::editing` 命名空间及准确的 StateId 语义保留；不为了目录名字改成另一套相同强类型。

实际迁移顺序：先补历史纯化测试；将 `EditTypes.cpp / EditHistory.cpp` 和其纯历史私有实现迁到 `editor/history/`；一次性修改 CMake/包含关系。原 `editor/editing/` 中领域编辑适配和 Close/Asset 协议不随目录批量删除，按账本在 P02/P12 处理。

### C1. 从历史中确实删除的 API 和字段

- `EditHistory::beginSave()`、`EditHistory::finishSave(...)` 的声明与实现。
- `editing::SaveTicket`、`ESaveOutcome`、`EHistoryEvent::SAVE_STARTED`、`HistoryCreateInfo::initially_saved`。
- `HistorySnapshot::saved`、`save_pending`、`clean`；以及实现中维护这些字段的私有计数、票据、状态与分支。内部限定名按 P00 的 AST 清单逐个删除，不猜名字。
- `EEditError::SAVE_IN_PROGRESS / STALE_SAVE` 从历史域移出；真正保存失败到对应保存模块，旧产品过渡用 Legacy 类型。其他 target 路由错误是否仍被历史使用必须依真实引用判断，不能顺便全部改枚举。

`HistorySnapshot` 留下 current/state/history/revision/event_sequence、条目与预算、closed；dirty 不在 History 的 observer 中再复制一份。历史保存点被裁剪也不改 checkpoint 的内容身份；撤销回当前可到达保存状态会 clean，不能达到时仍按身份比较。

### C2. 保持旧产品可构建，不允许双写 saved

所有旧 `beginSave/finishSave/initially_saved/snapshot.clean` 消费者在本阶段迁到限期 `LegacyPersistenceState`，包括 TAssetSave、Scene/Material/FlowForge、历史命令呈现和活动测试。该桥只组合新的 PersistenceCheckpoint 与单个旧保存票据，读取纯 EditHistory 的 StateId；不得给 History 加回隐藏 saved，也不得创建第二个 SavedTracker。

旧工具当前内容、候选内容、Save As 暂存历史若有切换，history 与其 checkpoint 必须成对采用/回滚；不能让旧 checkpoint 继续指向新 HistoryId。运行调试历史不自动获得资产保存基线。桥不拥有 EditorContext、不做 IO、不重新实现编码和线程调度，任何新模块不得包含它。P12 全部删除。

这是一处有期限的兼容，而不是维护两套历史实现。P00 账本列出的所有旧字段访问都须在本阶段解决，不能暂时把新 HistorySnapshot 的 clean 固定返回 true。

## D. 会话类型的精确契约

`SessionId` 由 Store 预留，含不可复用的代际；槽位释放后相同索引的新对象不能被旧 key 命中。`SessionKey<T>` 不公开可随意从整数构造的接口。Store 发布前验证 kind 与实际类型注册一致；未发布候选不能被查询到。

`IEditSession` 只承担异构集合所需的描述与关闭许可。Scene/Material/FlowForge 之后分别公开继承；具体 apply/undo/capture 不放入这个基类。Store 不弹对话框、不读取文件、不选择视图；业务源 unique_ptr 只在 Store 一处拥有。

`PersistenceCheckpoint` 记录 `optional<PersistedState>`，其中 StateId、BindingRevision、已采用发布序号必须同时变化。它不存存储后端对象或 SaveService 指针；发布序号使用 sessions 边界的小值，后端 receipt 由适配器在 P05 验证后转换。初始打开成功可建立“已持久化”基线，未命名新内容没有基线。不要以 revision==0、空路径或默认 expected 成功表示未保存/未开始。

`EditScope` 不可复制且不可移动，因此不要写一个无法编译的 `expected<EditScope>` 返回示例。采用 `EditGate::withEdit(callable)` 在检查成功后就地构造局部 scope，或使用私有构造+调用者既定栈作用域；错误发生前不得先设置 busy。gate 是唯一准入状态，业务旁边不能再有 `editing_busy`。

ClosePermit/BindingChangePermit 是 move-only，允许跨 owner 阶段，但 Store/许可状态必须长于所有 permit。关闭 permit 固定 SessionId+ContentStamp；解除许可必须核对同一代际。许可本身不是用户选择，不带“取消退出”的 UI 含义；关门期间仍允许只读 describe/status。

## E. 注册和析构顺序

Store 槽记录先声明 CodeLease，后声明 `unique_ptr<IEditSession>`，逆序析构对象再释代码。会话组合中的 source 必须长于其历史 memento，声明顺序与显式 close 顺序均要证明。SessionAccess 暂借只在 owner 线程同步范围有效；不允许保存到 future/callback。

`SessionReservation` 的放弃只归还未发布槽，不产生关闭通知。发布准备可失败；真正 publish 不得再调用任意插件构造或分配需要恢复的资源。角色配套的 InstalledSession 以后由用例层组合，Store 不塞入任意服务表。

## F. 本阶段构建与使用证明

建立 `editor_contracts → 无 Editor依赖`、`edit_history → editor_contracts`、`edit_sessions → editor_contracts/edit_history`。测试构造最小 FakeSession 实现 IEditSession，并完成 reserve/publish/access/close；它是测试替身，不是未来业务万能基类。历史与 SessionStore 的最小消费者不能链接 UI、GPU、ProjectStorage、SceneRuntime。

阶段末旧产品仍使用原工具，但共享历史机制已经是纯化后的唯一实现；新会话基础有真实测试。P02—P04 的新模型不再消费过渡桥。不要提前创建空 SceneSession 或完整 EditorApplication。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| editor/editing/src/EditHistory.cpp、EditTypes.cpp 及纯历史头 | 移动实现，不复制；更新所有构建/安装引用 | editor/history；纯历史算法保留；P01 | 已验证算法、资产兼容读取与行为回归不删 |
| EditHistory::beginSave/finishSave；SaveTicket；ESaveOutcome；SAVE_STARTED；initially_saved | 删除声明、定义、枚举与所有原调用；旧消费迁往限期桥 | PersistenceCheckpoint；LegacyPersistenceState 仅过渡；P01 | 已验证算法、资产兼容读取与行为回归不删 |
| HistorySnapshot::saved/save_pending/clean 与实现对应私有状态 | 从纯历史删除；禁止新旧基线双写 | SessionState::PersistenceCheckpoint；P01 | 已验证算法、资产兼容读取与行为回归不删 |
| LegacyPersistenceState / LegacySaveTicket / LegacyPersistenceOutcome | 本阶段新增限期旧产品桥；只调用新 checkpoint | P12 随最后旧工具删除；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| EditorError.hpp、CloseRequest.hpp、AssetEditing.hpp、领域 editing 目录 | 暂留给未切换的旧产品；禁止新模型依赖 | 错误分别进各域；协议由 P12 用例替换；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X01-01 / `history_has_no_persistence_members` | 编译公开历史头并扫描 API/成员 | 不存在 beginSave/finishSave/saved/clean；原编辑 prepare/commit 测试仍通过 |
| X01-02 / `session_slot_generation_and_type` | 删除后复用槽位，并尝试错类型 key | 旧 key/type 失败且不返回新对象 |
| X01-03 / `checkpoint_after_undo_and_reload` | 建立保存点后 edit/undo，再整体换 HistoryId | 撤销回保存点 clean；旧世代回执拒绝 |
| X01-04 / `scope_and_permit_lifetime` | 编译复制/移动 EditScope 的负例；移动及释放 permit | scope 非复制移动；permit 释放恰一次；无错代际解锁 |
| X01-05 / `legacy_bridge_uses_single_checkpoint` | 旧工具保存失败/成功/候选失败各一次 | History 不保存 saved；桥与内容切换一致，不复制第二份基线 |
| X01-06 / `session_candidate_failure_no_publish` | 候选、代码 lease 或槽位预算故障 | 无可见半会话；析构在代码卸载前发生 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P01.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
