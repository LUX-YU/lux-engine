# P03 — Material 作者图与独立会话

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P02。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M05；验证通用 Session/History 不是 Scene 专用。**关联问题：** A02、A06、A08、T06。**V3 回归：** Q01, Q07, Q09, Q11, Q27。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `MaterialSession` | `editor/tools/material/model/include/lux/engine/editor/material/MaterialSession.hpp` | final : sessions::IEditSession；组合 source/state/history | describe/read/apply/undo/redo/capture；不持有 preview/compiler/window。 |
| `MaterialSource（复用 lux::material::MaterialSource）` | `既有 modules 中定义；model 只做需要的受限适配` | 复用已有领域数据 | 不再造一份同名源并双向同步；作者图布局若被序列化仍属于源。 |
| `MaterialReadView / MaterialSnapshot` | `editor/tools/material/model/include/lux/engine/editor/material/MaterialSnapshot.hpp` | 短借用 / 不可变拥有值 | 节点、参数/纹理槽、状态与图布局均可准确捕获；无渲染句柄。 |
| `MaterialEditBatch / MaterialEditReceipt` | `editor/tools/material/model/include/lux/engine/editor/material/MaterialEdit.hpp` | 具体领域值 | 校验引脚类型/连接/节点与槽引用；提交一次历史。 |
| `MaterialGraphEdit / MaterialValueEdit` | `editor/tools/material/model/src/edits/` | 保留 EditOperation 协议继承 | 逆操作拥有真实节点/值与代码寿命，不引用 GraphElement。 |
| `MaterialSessionAccess / PreparedMaterialReload` | `editor/tools/material/model/include/lux/engine/editor/material/MaterialSessionAccess.hpp` | 受限访问 / 私有候选 | 重载保留 SessionId、更新 HistoryId，候选失败不改源。 |

## C. 将模型从预览和控件中取出来

当前 MaterialEditor::Impl 同时拥有 source_、history_、preview_、compilation_、save_、close_request_ 与 content_。新 MaterialSession 只能接纳前两者及 SessionState。不能因为材质“需要显示效果”就让 MaterialSession 构造时创建 SceneRuntime 预览实例。

本阶段必须实际复用 `lux::material::MaterialSource`、NodeId、PinId 与图数据格式；不将 SceneSession 泛化成 TemplateDocument<Everything>。Material 必须能在不启动窗口、不创建 RenderRuntime、不找 ProjectStorage、不编译 shader 的条件下独立编辑。

## D. 逐函数迁移契约

| 旧函数 / 类型 | 新入口 | 行为保留与失败语义 |
| --- | --- | --- |
| `canEdit`、`busy_` | SessionState::EditGate | 不保留第二枚 busy；不可重入规则与 Scene 相同 |
| `rename`、`setShadingModel`、`setRenderState` | MaterialSession::apply 中具体 value edit | 检验枚举/状态；NO_CHANGE 不新增历史 |
| `setConstant(node,value)` | MaterialSession 的节点值编辑 | 节点存在、类型/分量正确；不把预览编译成功作为合法编辑前提 |
| `setTextureSlots(base,slots)`、`setParameterSlots(base,slots)` | 带基线验证的声明编辑 | 引用更新与逆操作一次准备；不能部分替换数组 |
| `replaceNode`、`insertNode` | 节点替换/插入 batch | 成功才转移所有权；失败仍由调用者或候选拥有，不能泄漏 |
| `removeNodes(nodes,links)` | 删除批次 | 连接/节点引用一致，undo 能重建原 NodeId/PinId 对应关系 |
| `connect/disconnect` | 图连接编辑 | 类型、方向、重复/环策略沿用既有合法规则，失败不改图 |
| `moveNode/moveNodes` | GraphNodeLayout 持久编辑 | 已保存的节点位置继续可保存/撤销；屏幕 pan/zoom 属于 P10 视图 |
| `editGraph/GraphDelta/TValueEdit/GraphEditOperation` | model/src/edits | 保留预算、节点寿命、prepare/commit；不以 GraphElement 当模型 |
| `source/historyId/historyView/undo/redo` | MaterialReadView + Session.describe + 领域 undo/redo | 窗口历史命令以后由 HistoryActions 角色接入 |
| `requestCompile/compiled/compileStatus/requestPublish` | P07 编译/产物发布服务 | 不放进 IEditSession 或作者 dirty |
| `createPreview/resetPreview/updatePreview/maintainPreview` | P07 MaterialPreviewStore | 编译失败保留作者修改 |
| `previewCamera/previewInstance/navigatePreview` | P10 ViewState + Preview 呈现 | 无作者源副作用 |
| `requestSave*/changeAsset/read_result_/candidate_*` | P05/P12 | 模型只提供 capture / 受限原子候选采用 |

## E. 不要遗漏的语义区别

MaterialSource 的 `id` 是持久化根身份，NodeId/PinId 的语义必须在 Save As 前盘点清楚；Node 编辑 memento 不得直接捕获“旧文件根 ID 永不变”的假设。保留 Node 对象的动态类型与 allocator/deleter 代码寿命。

纯编辑允许产生编译器会诊断的中间图，但必须遵守本来就属于编辑器结构合法性的约束；不擅自把所有编译限制提前变成编辑拒绝，也不删除原结构验证。把“结构有效、尚未编译”和“编译成功”分别描述。

明确哪些数据是文件事实：节点坐标若现有编码保存它，就是作者图布局；预览相机、滚动、悬停和选中列表不在 MaterialSnapshot 里。快照和继续编辑共享数据时审计节点深度不可变，不能只在外层 const。

## F. 阶段出口

新的会话经过不同于 Scene 的图操作验证，证明 SessionStore、checkpoint、history 协议没有假设 ECS Entity、场景实例或渲染。如果通用契约不得不增加一条 Scene 特例才支持 Material，本阶段先修正 P01 公共契约及其全部已存在消费者，形成明确 ADR；不偷偷往 contracts 加 Material 类型。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| MaterialEditor::Impl::source_/history_ 与 GraphDelta/GraphEditOperation/TValueEdit 业务算法 | 迁到 material/model；旧图控件只留消费适配 | MaterialSession / MaterialGraphEdit；P03 提取；旧壳 P12 删除 | 已验证算法、资产兼容读取与行为回归不删 |
| MaterialSession 中 preview_/compilation_/save_/close_request_/content_/editor_context_ | 禁止引入 | 分别 P07/P05/P12/P10；P03 | 已验证算法、资产兼容读取与行为回归不删 |
| MaterialEditor.hpp / MaterialEditorImpl.hpp / MaterialAssets.cpp | 旧产品暂留且列全成员去向 | model/persistence/preview/interaction/ui；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X03-01 / `material_graph_without_preview` | 无 GPU/窗口/编译器创建材质并操作节点/连接/参数槽 | 编辑与 undo/redo/capture 可用 |
| X03-02 / `material_owned_node_transfer_failure` | 插入/替换节点准备阶段失败 | 节点无泄漏/双删；原图和 history 未变 |
| X03-03 / `material_graph_positions_are_authored` | 移动节点并保存快照，另改视图 pan/zoom | 持久布局进入快照；视图变换不污染 source |
| X03-04 / `material_snapshot_and_plugin_lifetime` | 快照后替换动态节点，晚析构旧快照 | 冻结值不变，插件代码覆盖最后析构 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P03.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
