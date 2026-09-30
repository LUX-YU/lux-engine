# P02 — Scene 作者模型、稳定对象身份与可撤销编辑

**实施包 V4 · 2026-09-28 · 调查基准 `2bb33ff1a1f11025cf404e074c0e9b259239d8a4`**  
**前置门槛：** P01。序贯交付时必须同时满足所有较早阶段的验收，不能因本阶段直接依赖较少而跳过中间文档。  
**责任域：** M04；复用 M02/M03。**关联问题：** A02、A08、T05、T06、T09。**V3 回归：** Q01, Q06, Q09, Q11, Q22, Q26。

> 本文是实施指令，不是已完成报告。旧符号依据固定基准；新路径和 API 是规定的目标，不冒充仓库已有内容。开始前读取实际 HEAD 与前阶段交接记录。若旧符号已由前阶段迁走，沿迁移记录定位新位置，不重新创建旧文件。本文可单独交给实施 LLM；完整包中的总契约、类型索引和删除账本用于交叉校验。

## A. 执行边界与交接前提

P00 首次执行按下文 C0 初始化账本，不要求不存在的前阶段收据；其余阶段先确认 `docs/editor-redesign/receipts/` 的前阶段记录与当前代码祖先关系，读取 `architecture.json`、`migration-ledger.json` 和 `test-coverage.json`。没有相应证据时只完成核验和缺口报告，不把未验收阶段当成已经通过。存在用户未提交修改时保留，不 reset、不覆盖、不清理无关文件。

本阶段只实现下面列出的职责；不得顺手重写 Lua、渲染后端、执行器、资源格式或全局插件系统。允许复用算法，不允许新模块 include 旧 Context/Editor 大类。禁止新增 `NewEditor`、`V2Session`、`LegacyManager` 等永久旁路。过渡代码必须进入唯一 `editor/transition/` 白名单、有调用方、单向依赖和不晚于 P12 的删除期限。

新增声明必须与实现、真实调用方或行为测试一起交付。只声明接口、默认返回成功、永久返回 Unsupported、空 `update()`、仅用 forwarding wrapper 包住旧 owner，都不算完成。接口草图使用 C++20 与现有 `lux::cxx::expected`，不要求升级标准；需要分配的函数不得未经处理一律标 `noexcept`。

## B. 本阶段类型、文件和责任清单

路径按仓库根目录解释。表内归为“复用”的类型不再造同义 wrapper；表内多种小值允许放在同一语义头，禁止堆入全局 `Types.hpp`。公开头放 `include/`，仅实现需要的状态放 `src/` 私有头，不重新开放兄弟模块的 pinclude/sinclude。

| 类型 / 角色 | 目标文件 | 组合、继承与 owner | 必须实现的 API / 不变量 |
| --- | --- | --- | --- |
| `SceneSource` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneSource.hpp` | SceneSession 值/独占组合 | 唯一可写作者事实；保留 WorldObjectId、schema、分区、配置；不是运行 Registry。 |
| `SceneSession` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneSession.hpp` | final : sessions::IEditSession | 组合 SessionState、SceneSource、EditHistory；describe/read/apply/undo/redo/capture。 |
| `SceneReadView / SceneSnapshot / SceneChangeSet` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneSnapshot.hpp` | 短借用 / 冻结拥有值 / 有界变更值 | 短借用不能跨 tick；Snapshot 深度冻结插件数据并保活代码；变化裁剪有 ResetRequired。 |
| `SceneEditBatch / SceneEditReceipt / SceneEditError` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneEdit.hpp` | 领域意图和值 | 一次批量编辑一个历史提交；precondition 使用 ContentStamp 或 StateId。 |
| `SceneSessionAccess` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneSessionAccess.hpp` | 复用 SessionAccess<SceneSession> 的窄别名/适配 | 读与编辑能力分配给不同消费者；无 engine()/panes()/project()。 |
| `SceneObjectRef / SceneObjectLocator` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneObjectRef.hpp` | 作者对象身份值 | 复用已有 WorldObjectId；跨会话引用带 SessionId/HistoryId；禁止裸 ECS Entity 作为持久作者地址。 |
| `SceneObjectEdit / SceneFieldEdit / SceneConfigurationEdit` | `editor/tools/scene/model/src/edits/` | 继承既有 EditOperation/PreparedEdit | 准备候选、预算、逆操作；无 Context、Pane 或可写运行世界；不要从 UI 借 memento。 |
| `PreparedSceneReload（内部值）` | `editor/tools/scene/model/src/PreparedSceneReload.hpp` | 私有候选 | 源、全新历史、checkpoint 和索引一起准备；失败保留旧会话。 |

## C. 模型的真实存储与依赖边界

不能把旧 `SceneEditor::Impl` 整体改名为 SceneSession。新会话只拥有作者内容、历史、身份与保存基线。`SceneRuntime` 的 registry 可以用于派生呈现，但不能是新作者模型对外的可写事实来源。SceneSource 可以复用 `detail::SceneContent`、ScenePackage 数据描述、现有 schema/分区和 CPU 容器，不要求重写 ECS 或文件格式；它不得在构造时创建引擎运行实例或加载 GPU。

已有 `WorldObjectId` 是 UUID 身份，位于 `engine/domain/world/identity/include/lux/engine/world/WorldObjectId.hpp`。本阶段直接复用，不造 `AuthorEntityId` 与其长期互相转换。确需运行 Entity 映射时只在后续 projection/execution 适配中维护。项目中已有 `SceneContent` 与 `WorldMaterializer` 等算法必须先分清源数据/运行投影两部分再搬运。

作者内容中的对象布局、组件、系统配置、分区/世界描述、仿真配置如果已参与现有编码，继续属于作者源。编辑视口相机、网格平面、选中对象、活动 Inspector 与 viewport 输出不能混进文件。某字段是否持久化以实际 codec 为证据，不按变量名字猜。例如 viewport_system_ 的“选择哪个作者渲染系统”和“当前窗口看哪个输出”可能需拆为领域配置与视图选择，不能整块移动后丢掉原序列化行为。

### C1. 必须实现的领域 API

| 操作 | 新入口与契约 | 对历史/失败的要求 |
| --- | --- | --- |
| 创建空/配置场景 | owner 线程的 SceneSession 工厂，输入预留 SessionId、SourceBinding、已验证源配置 | 创建候选未发布；初始 StateId 明确，Untitled 无持久化基线 |
| 查询对象、父子、组件 | `read()` 返回 SceneReadView，访问器只读且限定同步作用域 | 不返回可修改 Registry、不以 void* 任意写组件 |
| 新建/删除/重挂父对象 | `apply(SceneEditBatch)` 的具体领域意图 | 验证全批次后一次提交；失败没有部分对象或半条历史 |
| 字段修改 | 按稳定对象、组件 schema 和字段路径准备 SceneFieldEdit | 校验目标历史/字段类型，逆操作持有所需值和代码 lease |
| 模型插入 | 接收已加载且拥有寿命的 ModelAsset 与放置参数，准备整组作者对象 | IO 不在此 API 内；分配全部准备后一次发布 |
| 从视图创建相机 | 输入相机参数值生成作者相机对象 | 不借 SceneView 或临时运行 camera Entity；一次历史编辑 |
| undo/redo | 具体 SceneSession 方法，经过唯一 EditGate | 只改变该源及其历史；失效对象与资源还原语义保持 |
| capture | `capture(SnapshotBudget)` | 冻结已提交状态；不捕获半个拖拽预览；预算不足返回失败 |
| changesSince | 返回有界增量或 ResetRequired | 不能因漏一次 signal 永远不同步；版本与历史身份一起校验 |

函数签名方向（非可直接编译的整套 SDK）：

```cpp
class SceneSession final : public sessions::IEditSession {
public:
    sessions::SessionInfo describe() const override;
    SceneReadView read() const;
    SceneEditResult<SceneEditReceipt> apply(SceneEditBatch);
    SceneEditResult<SceneEditReceipt> undo();
    SceneEditResult<SceneEditReceipt> redo();
    SnapshotResult<SceneSnapshot> capture(SnapshotBudget) const;
private:
    sessions::SessionState state_;
    SceneSource source_;                         // 必须长于历史 memento。
    std::unique_ptr<editing::EditHistory> history_;
};
```

真实代码采用本项目的错误别名，不需要把这里的每个别名变成跨项目框架。`describe()` 不能返回可写 SourceBinding；除持久化/重载的受限访问类以外没有 markClean 或 replaceSource 公开后门。

## D. 旧成员和函数按职责拆除，而不是拷贝

本阶段将下面业务算法抽到新领域；旧产品尚未切换，只有旧调用壳可以暂留，复制大段业务到新旧两份不可接受。能让旧调用经一个受限适配复用纯算法就这样做；必须依赖旧 Registry 的适配仍留在旧文件并记录 P12 删除，不让新域反向 include 它。

| 旧 SceneEditor::Impl 内容 | 新归属 | 明确限制 |
| --- | --- | --- |
| `source`、`content`、`history`、作者配置 | SceneSource / SceneSession | `source` 里的不可变打包描述与可写作者数据要辨清，不能双向同步两份源 |
| `scene_editing`、`ParentEdit`、`ObjectEdit`、`makeParentEdit/makeObjectEdit` | model/src/edits | 保留 prepare/commit 协议，去掉 SceneEditor& 参数 |
| `createObject/eraseObjects/reparent/createEntitiesFromModel/executeContent` | SceneSession::apply 的具体编辑 | 用作者 ObjectId，不以 inspectedScene() 选择目标 |
| `component/writeTarget/checkStructure/fieldEditWritable/fieldEdited/finishFieldEdit` | 只读组件描述 + 字段编辑准备；拖拽结束归 P08 | 修改不通过拿出的可写指针绕过历史 |
| `structure_revision`、`componentVersion` | 模型变化版本或快照元数据 | 不能与 UI 刷新版本/dirty 共用一个计数 |
| `source/asset_source/render_receipt` 中渲染部分、`scene` 实例 | P07 SceneProjection | 不进入 SceneSession |
| `editor_camera/selection_/run_selection`、`work_plane_height` | P08 交互 / P10 ViewState | 不保存在作者源 |
| `run_scene/run_source/run_history/run_editing/run_*` | P06 RunStore 与运行调试模型 | 当前阶段禁止引入到 SceneSession |
| `save/change_save_/candidate_*/reading_/read_result_` | P05 持久化 / P12 Open/Reload 用例 | 领域 capture 不内含长期保存或加载任务 |
| `content_/inspector_/outliner_/resources_/creation_pane_` | P10 视图与宿主 | 不成为 SceneSession friend 或成员 |

## E. 源身份、快照与历史闭合

字段/结构编辑在 prepare 阶段完成校验、分配和逆操作构建；commit 只做已准备的状态改变。模型与 History 当前状态不能被观察到一个已更新、另一个未更新的中间点。失败场景覆盖第三个对象创建失败、父环、缺 schema、超过历史/捕获预算。

复制 Snapshot 时必须审计指针可达图：`shared_ptr<const ScenePackage>` 外层 const 不证明其内部插件节点不可变。选择实际的深复制、不可变节点或写时复制，并说明行为/成本；不在重构文档中承诺零复制。捕获中涉及的插件 schema、allocator、deleter 通过 lease 保活到最后产物销毁。

Save As 不在此阶段实现，但作者逻辑 UUID 不能依赖当前文件根 AssetId 不变；把打包引用映射限制到 codec。保持历史可在重绑定后 undo 是 P05 的前置设计约束，不许以后用 clearHistory 避开。

## F. 阶段出口

使用真实的小 ScenePackage/CPU 数据样本构造无 UI 的 SceneSession，完成新建、组件编辑、结构编辑、undo/redo、冻结快照和失败回滚。证明链接闭包无 `ui`、`editor_context`、`scene_composition`、GPU/窗口与 ProjectStorage 实现。资产 identity/description、数学与纯 CPU schema 依赖允许且必须如实导出。

本阶段不是把产品显示切换到新源；它交付可独立测试的实际作者模型。只有 P12 产品切换后才能删除整个旧 SceneEditor 文件，但新模块从本阶段起不能出现旧类型引用。

## G. 本阶段删除与暂留边界

| 旧文件 / 符号 | 本阶段动作 | 最终去向 / 删除期限 | 不得误删 |
| --- | --- | --- | --- |
| SceneEditor::Impl 的 ParentEdit/ObjectEdit 与纯作者编辑算法 | 提取到唯一纯实现；旧调用只能通过限定的算法适配 | scene/model/src/edits；P02 完成提取；旧壳 P12 删除 | 已验证算法、资产兼容读取与行为回归不删 |
| scene/model 中的 EditorContext/SceneEditor/Pane/SceneRuntime 依赖 | 本阶段禁止，出现即不通过 | SceneSession 只依赖作者域与 history/sessions；P02 | 已验证算法、资产兼容读取与行为回归不删 |
| SceneEditor.hpp、SceneEditorImpl.hpp 和旧 SceneContent/SceneObjectEdits/ModelCreation 文件 | 旧产品暂留，逐项记录去向；不是新 API 的依赖 | model/projection/interaction/ui/persistence/execution；P12 | 已验证算法、资产兼容读取与行为回归不删 |
| 已有 WorldObjectId、场景编码算法、EditOperation/PreparedEdit | 复用，不重命名造第二份身份/编辑协议 | 原 engine 纯域和 history；保留 | 已验证算法、资产兼容读取与行为回归不删 |

删除操作必须同步覆盖：声明、定义、调用方、friend、注册生成输入、CMake source/target、导出与安装清单、示例、活动测试和文档。改扩展名、挪到 backup、注释整段、`#if 0`、`EXCLUDE_FROM_ALL`、残留 alias 都不算删除。用户资产/布局备份、版本化旧格式只读解码器和负向测试样本不属于垃圾代码，按独立保留清单保护。

## H. 验收场景与反作弊检查

| ID / 测试名 | 输入、故障点或操作 | 必须观察到的结果 |
| --- | --- | --- |
| X02-01 / `scene_session_headless_real_content` | 不创建 Root/Runtime/GPU，用真实 CPU 场景建立会话 | 结构/字段编辑、undo、capture 可用，公开依赖闭包干净 |
| X02-02 / `scene_edit_batch_is_atomic` | 多对象插入时第三处预算失败；重挂父形成环 | 所有作者对象和历史游标保持；没有半提交 |
| X02-03 / `scene_snapshot_freezes_plugin_payload` | 捕获后继续修改节点并延迟释放插件加载句柄 | 冻结内容不变，deleter 执行前代码仍存活 |
| X02-04 / `author_identity_survives_storage_rebind_design` | 不同打包根地址使用相同作者 UUID 场景样本 | 领域寻址不依赖运行 Entity 或当前持久 AssetId |
| X02-05 / `changes_since_overflow_requires_reset` | 裁剪增量记录并整体换 HistoryId | 明确 ResetRequired，不返回伪完整增量 |

同时执行本阶段相关 V3 场景与此前受影响的回归。只有“新增测试文件”不算执行；只有 fake 通过不替代该阶段要求的真实 IO、桌面或 GPU 结果。编译负向用例必须证明失败原因正是禁止依赖/非法操作，不能因缺少第三方头而误判通过。

## I. 交付格式、允许停点与禁止续行

提交按“契约与测试 → 实现和调用方迁移 → 旧代码删除与构建/安装更新”组织，但阶段末必须是可构建、可验证的整体。写入 `docs/editor-redesign/receipts/P02.json`，记录 `implementation_sha`、实际命令、测试结果、对应日志路径/校验和、新 owner、已删除项与有期限的暂留项。`implementation_sha` 指测试过的实现提交；收据在之后的证据提交写入，避免把收据自己的 SHA 写进自身形成循环。

输出中文报告：完成了哪些行为；哪些真实失败仍存在；删除了哪些旧符号/文件；剩余项属于哪个后续阶段；是否满足本阶段全部门槛。状态只允许 `PASS / BLOCKED / PARTIAL`；缺少必须环境或某项必测未运行时不能写 PASS，不能自动进入下一阶段。失败测试、崩溃日志和负结果保留，不改成 SKIP、降低断言或删除功能来凑通过。

提交前运行迁移审计脚本并人工核对 AST/引用清单。脚本只能检查文本、路径与清单，不能证明所有权正确，也不能代替编译和运行。最后检查 `git diff --check`、当前工作树、安装导出及阶段期限；不执行本文件之后的下一阶段。

**配套规则：** [总执行契约](../00_EXECUTION_CONTRACT.md)、[类型索引](../appendices/A_TYPE_INDEX.md)、[逐成员迁移账本](../appendices/B_RETIREMENT_LEDGER.md)、[测试映射](../appendices/C_TEST_MATRIX.md)、[固定源码证据](../appendices/E_SOURCE_REFERENCES.md)。

**交给下一阶段的最小材料：** 本阶段代码与证据提交、上述收据、更新后的迁移账本与测试覆盖表、已知限制。下一位 LLM 不需要猜测本轮聊天中没有写进仓库的决定。
