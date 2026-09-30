# P09 — 布局、会话恢复与偏好的模型和持久化

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P05, P08。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M12；P12 ApplyLayout 用例的纯输入。**关联问题：** T01、T02、C01、C02。**V3 回归：** Q04, Q31, Q32, Q33, Q35, Q36, Q37, Q52。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `LayoutId / LayoutSlotId / DockLayout / DockTree` | `editor/workspace/layout/include/lux/engine/editor/workspace/DockLayout.hpp` | 持久纯值 | 布局组织不含作者源、运行实例或 live SessionId。 |
| `ValidatedLayout / LayoutPlan / LayoutPlanner` | `editor/workspace/layout/include/lux/engine/editor/workspace/LayoutPlan.hpp` | 验证后值与纯计划 | parse→validate→resolve；不修改 Root，不创建活动视图。 |
| `LayoutCatalog / LayoutSummary / CatalogVersion` | `editor/workspace/layout/include/lux/engine/editor/workspace/LayoutCatalog.hpp` | 查询快照 | 目录不是请求；没有 pending/result/action 组合袋。 |
| `RecoveryManifest / RecoveryEntry` | `editor/workspace/recovery/include/lux/engine/editor/workspace/RecoveryManifest.hpp` | 独立恢复描述 | 存可持久定位与视图绑定；不声称能恢复未持久化修改。 |
| `UserPreferences / VersionedViewState / PreservedOpaqueState` | `editor/workspace/layout/include/lux/engine/editor/workspace/WorkspaceValues.hpp` | 独立值/版本化载荷 | 未知 type/schema 字节保留；偏好坏引用可回退。 |
| `WorkspaceStore / LayoutCommitReceipt / PreferenceWriteResult` | `editor/workspace/storage/include/lux/engine/editor/workspace/WorkspaceStore.hpp` | 真实布局/偏好 IO 适配 | save/rename/remove/read/list 各有准确事实，偏好写失败不撤销布局成功。 |
| `LegacyWorkspaceImporter（只读格式适配）` | `editor/workspace/storage/src/LegacyWorkspaceImporter.cpp` | 保留的数据迁移边界，不是旧 owner | 解析旧数据到新值；不 include WorkspaceRequest/旧 PaneManager，不执行未知 provider。 |

## C. 四个对象的边界必须体现在接口上

DockLayout 只有布局身份/label、槽位、DockTree 和纯视图配置；RecoveryManifest 记录持久内容定位与恢复关联；UserPreferences 记录选中的 LayoutId 等偏好；LayoutCatalog 只描述目录。不能再封进一个 WorkspaceData 作为读写一体的公共协议。

命令用独立小值/函数：SaveLayout(DockLayout)、RenameLayout(LayoutId,label)、RemoveLayout(LayoutId)、ReadLayout(LayoutId)、ListLayouts。方法返回各自的值/receipt。IO 正在进行的状态属于调用方操作记录，不放在 DockLayout 或命令结构中。

## D. 纯验证与计划

先解析所有 dock 节点和槽位，再验证 schema、重复身份、引用完整性、树无环、比例有限且合法、窗口类型约束。坏 dock 数据必须在任何活动 UI 修改之前被发现。

LayoutPlanner 的输入是 ValidatedLayout、现有 ViewInfo 快照和已固定的 provider 描述集合（本阶段可用测试 provider catalog；P11 将真实扩展 snapshot 适配进来）。输出精确匹配、创建未绑定外壳、保留额外视图、未知载荷与遗漏列表；不读取具体 Scene/Material 内容。

复用按 ViewRestoreKey+ViewType 匹配，不能同类型拿第一个就重新绑定它。已有绑定和未保存内容保持；额外窗口默认保留。布局不重开资产；恢复内容必须由 P12 独立 RestoreSession/Open 用例执行。

## E. 真实存储格式与一致性

采用 `layouts/<LayoutId>.layout`，label 在文件内；RenameLayout 修改一个记录而不是重命名文件并同步 settings。偏好只引用稳定 LayoutId。文件编码可以继续选 TOML/JSON/现有二进制工具，但必须写 schema 版本、类型/载荷长度及预算，不能把 C++ struct 内存直接持久化。

布局提交 receipt、偏好结果、目录刷新结果分别报告。RemoveLayout 已完成后偏好清理失败，删除事实仍保留；下次启动发现活动 ID 不存在，回到明确默认布局并输出可诊断原因。禁止因一个偏好坏引用让整个项目无法启动。

本模块的文件发布同样准确区分实际发生与未发生；复用已有可靠文件发布基础，不再复制 .next/rename 的各种近似实现。WorkspaceStore 固定借用 P05 的同一个 WriteCoordinator；用不同的受控目录命名域形成 WriteTargetKey，由规范化后端保证同一物理目标不能被两种命名域绕过。workspace_store 因而显式依赖 editor_persistence。不要另建 WorkspaceWriter 或让 SaveService 的普通资产接口保存布局。

## F. 旧格式迁移的精确规则

LegacyWorkspaceImporter 只保留旧 schema 的最小数据解析。已识别布局几何进入 DockLayout，内容 locator 进入 RecoveryManifest，当前选中项进入 UserPreferences。未知字段带原 type/schema/原始 payload 保留，不 reinterpret_cast 成当前类型。

首次迁移生成稳定 LayoutId 映射，写出新记录并校验，再发布迁移完成标记；崩溃重试应幂等。映射不能每次启动随机生成导致重复布局。旧文件保留为只读备份，禁止新旧格式双写；旧 backup 是用户数据，不在垃圾代码删除规则中。

如果 importer 发现某旧记录实质包含未保存内容但仅提供 locator，明确报告不能恢复其修改；不编造自动保存能力。大小/递归预算与未知载荷 roundtrip 测试必须覆盖。

## F2. 阶段出口

本阶段完成纯模型/计划和真实存储，不实现实际 Root 修改。P10/P12 使用其结果挂载/应用。旧 EditorWorkspaceStorage 仍仅服务旧产品到 P12；新模块不得 include 旧 WorkspaceData/WorkspaceRequest，迁移器使用独立私有 legacy schema 值而非旧业务类型。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| 新路径中的 WorkspaceRequest / EWorkspaceAction / WorkspaceData | 禁止依赖，分成纯值、IO 返回与用例操作 | DockLayout/LayoutCatalog/RecoveryManifest/UserPreferences；P09 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧 EditorWorkspace.cpp/EditorWorkspaceStorage.cpp/EditorWorkspace.hpp | 旧产品暂留；解析算法仅以私有只读迁移器复用 | P12 新 ApplyLayout 接管后删除旧业务文件；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 旧布局备份、未知 type/schema payload、迁移映射 | 必须保留可逆读取/审计信息；不是旧业务架构 | LegacyWorkspaceImporter + 用户数据备份；保留 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X09-01 / `invalid_dock_is_rejected_before_plan_effects` | 完整视图条目配坏 dock/重复 ID/环 | 纯验证失败，任何 UI/Session 计数不变 |
| X09-02 / `rename_layout_with_unwritable_preferences` | 修改 label 成功，prefs 权限拒绝 | 同 LayoutId 仍可选择，报告独立结果 |
| X09-03 / `delete_selected_layout_then_restart` | 删除成功但偏好仍指向旧 ID | 回退默认且报告，不启动崩溃 |
| X09-04 / `opaque_state_roundtrip_and_budgets` | 未知 provider/未来 schema/大载荷 | 合法未知字节保留；超预算准确拒绝不截断冒充完整 |
| X09-05 / `legacy_migration_crash_is_idempotent` | 新文件已写但标记前中断，再启动迁移 | 不重复生成新 ID；旧文件未破坏；仅新格式继续写 |
| X09-06 / `layout_plan_preserves_extra_views_and_content` | 现有 dirty Session 与布局外额外视图 | 计划保留它们且不触发 asset open/rebind |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P09.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
