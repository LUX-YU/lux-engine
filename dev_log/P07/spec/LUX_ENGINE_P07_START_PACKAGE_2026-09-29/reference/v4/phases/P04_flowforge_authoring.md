# P04 — FlowForge 作者图与第三工具覆盖

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01, P03。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** 新增 M17：FlowForge；遵守 M03/M06/M11 相同边界。**关联问题：** A02、A03、A08；补齐 V3 未单列的旧框架消费者。**V3 回归：** Q01, Q06, Q09, Q11, Q49。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `FlowSession` | `editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSession.hpp` | final : sessions::IEditSession | 组合 SessionState、Flow 作者源、EditHistory；无 compiler/linker/UI。 |
| `FlowAuthoringSource（仅当既有 FlowSource 不能表达可编辑图时）` | `editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowAuthoringSource.hpp` | 具体模型适配，不是又一份同步源 | 可复用旧 Content{id,name,FlowGraph}，改准确语义名；FlowSource 是编码/捕获值时不混同。 |
| `FlowSnapshot / FlowReadView` | `editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSnapshot.hpp` | 拥有快照 / 限时只读视图 | 变量、导出、函数签名、字面值、节点布局与环境寿命完整。 |
| `FlowEditBatch / FlowEditReceipt` | `editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowEdit.hpp` | 领域值 | 具体编辑不继承 MaterialSession；共享图 UI 不代表共享语义。 |
| `FlowGraphEdit / FlowVariableEdit / FlowLiteralEdit` | `editor/tools/flowforge/model/src/edits/` | 沿用 EditOperation/PreparedEdit | 保留可逆编辑与引用校验；不依赖 linker。 |
| `FlowSessionAccess / FlowSourceEnvironment` | `editor/tools/flowforge/model/include/lux/engine/editor/flowforge/FlowSessionAccess.hpp` | typed access；环境复用既有类型 | 元信息和 module lease 长于节点、history 与快照。 |

## C. 为什么这是必要阶段而不是新需求

当前仓库已有 `editor/tools/flowforge`，其 FlowForgeEditor::Impl 具有 source_、history_、EditorContext、TAssetSave、候选切换、编译链接、关闭协议和控件。删除 Context/AssetEditing/Pane 工厂之前必须把该工具迁移完；否则“最终旧架构已删”无法成立。这里是对 V3 覆盖范围的显式补全，不是要求增加 FlowForge 新功能。

## D. 精确拆分

| 旧内容 | 新归属 | 具体动作 |
| --- | --- | --- |
| `Content { id,name,graph }`、`source_` | FlowAuthoringSource 或现有可编辑源 | 只保留一份可写 graph；明确与冻结 FlowSource 的区别 |
| `environment_`、metadata() | FlowSession 注入的不可变环境与 lease | 持续到 source/history/snapshot 的最后析构；不通过 Context 临时取引用 |
| `read_nodes_/read_pins_`、NodeIndex/PinIndex、indexContent | Flow 模型内部派生索引 | 作者变更后刷新/失效；不得持有已删除节点指针跨修改 |
| `VariableEdit/LiteralEdit/TValueEdit/GraphDelta/GraphEdit/ExportsAccess` | model/src/edits | 准备/提交协议保持；去掉控件、Editor 与编译依赖 |
| `nodes/pins/links/nodeName/nodeOperation/pinName/pinType/nodeLayout` | FlowReadView | 明确借用期限；跨帧 UI 使用 owning snapshot |
| `setPinLiteral/rename` | FlowSession::apply | 类型校验与 NO_CHANGE 规则 |
| `insertNode/captureNode/insertFunctionUse/setFunctionSignature` | 具体图/函数编辑 | 更换签名时引用一致、失败无半图 |
| `exports/setExports` | 导出声明编辑 | 导出节点校验，逆操作完整 |
| `variables/addVariable/setVariable/removeVariable/variableReferenced` | 变量编辑规则 | 使用中的变量不能无策略删除；不靠 UI disable 作为唯一检查 |
| `removeNodes/connect/disconnect/moveNodes` | FlowEditBatch | 持久图布局保留，连接规则沿用真实领域定义 |
| `requestCompile/acceptCompilation/retryLink/compiled/requestPublish` | P07 FlowCompilationService / PublishFlowArtifactOperation | 作者模型不执行外部 linker |
| `asset_status_/reading_/candidate_/saved_history_/save_` | P05/P12 | 不搬进新 Session；history 保存基线仍只有一处 |
| `ContentElement/GraphElement/content_/close_connection_` | P10 FlowView | 只负责 UI/交互，不拥有作者图 |

## E. 环境与动态代码

FlowGraph 节点可能依赖动态元信息、函数工厂和代码。不能为了模型无 Context 就把 environment_ 改成裸全局单例。构造时注入确切只读环境/lease；使用它进行校验和节点构造。源和历史的析构先于环境释放，快照自身也保活其所需部分。

本阶段不引入新的 Lua VM、IR、代码生成器、脚本线程模型，也不修改引擎 FlowForge 执行语义。保留 `FlowSource`/ScriptArtifact 的现有编码与编译边界；编译需要 lld-link 是 P07 的真实 toolchain 测试条件，不能成为本阶段 native 图编辑测试的前置。

## F. 阶段出口

完成一个包含变量、函数签名、导出、连接与节点布局的实际 Flow 图：新建、编辑、失败、撤销、捕获均 headless 可用。三种具体会话可同时放在同一个 SessionStore 中，但任何 Session 基类都没有 compile/play/viewport 的 Unsupported 虚函数。

阶段末登记 `flowforge_model` target。其依赖只允许 sessions/history 与真实 FlowGraph/元信息的低层库；不得直接依赖 FlowForge 编译器实现、脚本运行时执行服务、UI 或 app。若纯数据定义目前和执行实现同 target，先拆出其已有纯数据边界，不能在新域继续 PUBLIC 链接整个执行器。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| FlowForgeEditor::Impl::Content/source_/history_/environment_ 与图编辑算法 | 迁到独立 Flow 作者模型；保留元信息寿命 | FlowSession / FlowAuthoringSource / edits；P04 提取；旧壳 P12 删除 | 已验证算法、资产兼容读取与行为回归不删 |
| FlowSession 对 EditorContext/Pane/FlowCompilation/linker 的依赖 | 禁止；编译与图编辑测试分层 | flowforge_model 与 flowforge_compilation 分开；P04 | 已验证算法、资产兼容读取与行为回归不删 |
| FlowForgeEditor.hpp / FlowForgeEditorImpl.hpp / FlowAssets.cpp | 旧产品暂留，不作为新模型实现 | P07/P10/P12 接管剩余职责；P12 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X04-01 / `flow_graph_without_linker` | 环境中无 linker，编辑变量/函数/导出并 undo | native 模型全部可用；没有通过禁用工具绕过 |
| X04-02 / `flow_referenced_variable_and_signature_failure` | 删除仍被引用变量；修改签名造成无效连接 | 按已确定领域策略拒绝/统一修改，失败没有半图 |
| X04-03 / `flow_environment_outlives_graph_and_history` | 释放加载器外部句柄后销毁快照/会话 | 节点与 memento 析构仍有有效代码 |
| X04-04 / `three_sessions_share_only_real_contracts` | 三种 Session 同 Store 查询、关闭准备与类型校验 | 无 Scene 特例、无 Unsupported 虚方法大基类 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P04.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
