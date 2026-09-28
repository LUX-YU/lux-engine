# P02 R1 复审结论与 P03 启动指令

**日期：2026-09-28**  
**项目：LUX-YU/lux-engine**  
**分支：codex/editor-redesign-v4**  
**复审 HEAD：`e0f440067792dc661e99a4f861a4116eeab4d304`**  
**功能补正：`8ef17a1014ca02265c535d7855cf6848b8f9b936`**  
**目录收拢及最终验收实现：`5d266dfa29877d4e226ac7d04ea38d55e4f3d86d`**

## 0. 放行决定与审阅边界

**P02 R1 本轮复审通过，允许进入 P03；仅授权 P03，不自动进入 P04，不合并 main。**

本轮核对了远端 HEAD 和提交链，阅读了功能补正与目录移动差异、最终关键实现、混合批次和 codec 回归测试、架构负例、冻结收据验证器及部分原始归档日志。没有发现必须在 P03 前再做一轮 P02 补正的前置问题。

这不是对任意场景的无缺陷保证。没有独立执行引擎全量构建、51 项 CTest、安装消费者或 GPU 测试，也没有独立复算全部归档文件哈希。曾尝试取得固定版本脚本用于额外的本地架构负例运行，但本地下载通道未能取得所需文件，测试未实际启动；这不是检查器或产品测试失败，不作为产品阻塞。文中运行成绩属于实施方提交的归档证据，源码复审与独立运行必须分开表述。

本文件是原 V4 `phases/P03_material_authoring.md` 的启动补充，不建立平行 V5 架构。原 P03 的领域目标、四项 X03 场景及删除期限继续有效；本文件补充实际起点、已经完成的路径调整、R1 接口和后续必须继承的语义。

## 1. P02 R1 复审结果

| 项目 | 已读实现与证据 | 结论 |
|---|---|---|
| 混合批次 | RemoveComponent 清除同 object/schema 的字段 delta，并移除对应 scratch 组件；后续字段从当时存在的新载荷重新建立临时值 | 上轮旧 scratch 覆盖重建组件的问题已针对性解决，未重建另一套批次系统 |
| 稳定读取 | 唯一 EditGate 增加 READING / withRead；capture 与 owning component read 进入相同 gate；私有栈作用域退出时释放 | 没有用编辑状态假扮读取，没有新增 capture_busy 或第二套准入 |
| 回归深度 | 比较完整组件编码、所有对象、历史、观察游标和 dirty；覆盖 undo/redo、非法顺序、异常恢复、close/rebind/undo/redo/reload 重入及捕获临时值析构 | 不仅检查 apply 成功或“不崩溃”；原局部字段不解码无关插件的测试保留 |
| 目录收拢 | history / sessions 物理移到 editor/editing；target、逻辑 include、命名空间、库名与 package 在目录移动中保持 | 没有把纯算法并回旧兼容 target；按主题归组与依赖边界可以同时保留 |
| 门禁 | 真实 CMake 负例采用新精确路径，并检查旧目录到期、旧保存 API、私有桥禁入和传递依赖 | 目录移动没有被用作路径检查豁免 |
| 历史证据 | 新脚本按每阶段 implementation_sha 物化 editor 源码，配合冻结证据运行原验证器；P00 部分明确仅验证可取得的源码和 fixture | 当前源码归属与历史来源分开，不改写原阶段成绩 |

读取到的回调重入归档对照：

```text
修复前：nested_apply=1
        snapshot_stamp_unchanged=1
        live_stamp_unchanged=0 snapshot_payload_unchanged=0

修复后：nested_apply=0
        snapshot_stamp_unchanged=1
        live_stamp_unchanged=1 snapshot_payload_unchanged=1
```

归档 CTest 为 51/51：原 43 项保留，新增八项混合/读取回归。README 记录 SDK 重装及六组、七项安装消费者通过；本轮没有独立重跑这些消费者。目录移动前后闭包及唯一编译的记录见 `dev_log/P02-R1/evidence/structure.json`，不要把该记录的布尔值单独当作语义证明。

原三项缺陷继续为 FAIL：C01 → P09/P12；C03 → P11；C04 → P12。不阻塞 P03，不在本阶段顺手修复。P02 已披露的索引 World 编辑限制、结构候选成本和预算非 RSS 上限也不升级成本阶段的新需求。

## 2. 开始 P03 前的交接

1. 核对实际 HEAD、工作区和祖先关系。上述 P02 R1 验收提交必须是起点或已验收祖先；有后续修改时先说明差异，不 reset 覆盖它们。
2. 读取原 P03、总执行契约、类型索引、当前迁移账本及 P00/P01/P01-R1/P02/P02-R1 记录。不重复实施已通过的历史、会话或 Scene 模型。
3. `.internal/editor-redesign/` 仍是唯一可变施工材料；阶段末冻结到 `dev_log/P03/`。原文档中的 `docs/editor-redesign/receipts` 示例不再形成第二份可变账本。
4. 更新当前施工规范中的路径映射，不修改冻结的原收据、日志和 SHA。普通编辑引擎回归、历史证据验证与当前迁移门禁是不同检查。
5. 已保全的 lambda 排版变化属于本轮功能提交，不回溯为原 P02 验收内容；下一轮不再按“待清理垃圾”删除它。

## 3. 目录与模块决定：继续收敛，不再散开

```text
editor/
  contracts/                       # 沿用，不为本阶段再搬一次
  editing/
    history/                       # 唯一 edit_history
    sessions/                      # 唯一 edit_sessions
    include/、src/、scene/          # 旧协议/适配，按账本期限暂留
  tools/
    scene/model/                   # 已通过复审，保留回归
    material/
      model/                       # P03 新领域；不是新顶层目录
      include/、pinclude/、src/     # 当前旧产品，逐项迁出/暂留
  transition/
    LegacyPersistenceState.*        # 原唯一桥，仍最迟 P12 删除
```

不得重新创建 `editor/history` 或 `editor/sessions`。链接继续使用 `edit_history` / `edit_sessions`；逻辑 include 继续使用现有 `lux/engine/editor/editing/...` 与 `lux/engine/editor/sessions/...`。

新 `material_model` 是真实领域边界，归属于 Material 功能包。不要为每个 edit、snapshot、helper 新建 target；不要同时更名全部安装包或改成新的 SDK 汇总工程。本阶段不规定“一模块一 DLL”，也不借减少目录把纯模型并入旧 `editor_material` / `editor_ui`。

若 Material 需要复用当前位于 Scene 包里的某个看似通用 helper，先核对其真实依赖和语义；禁止仅为几段值处理逻辑让 material_model 依赖 scene_model。确有共同机制时，将最小纯子集归到已有 editing 主题的适当位置并同步全部消费者；不得复制两套，也不得未经论证建立新的万能 Common 库。没有真实复用需要时不预先抽象。

## 4. P03 唯一业务目标

**让材质作者图脱离窗口、预览和编译任务，成为真正独立的 MaterialSession。**

真实 CPU 测试应能创建材质、插入/替换/删除节点、连接引脚、修改常量及声明槽、移动作者节点布局、撤销重做和捕获冻结快照；不创建窗口、不启动 RenderRuntime/SceneRuntime、不查询 ProjectStorage 实现、不编译 shader。

作者图结构有效与编译成功不是同一事实。保留现有图规则，不把所有编译诊断提前变成编辑拒绝；也不以“允许中间图”为由放弃既有结构约束。

## 5. 类型、关系与目标文件

下列是原 P03 的具体实施目标，不声明这些类型已存在。已有等价类型直接复用，不制造同义 wrapper。

| 类型/角色 | 目标位置 | 所有权与约束 |
|---|---|---|
| MaterialSession | `tools/material/model/include/lux/engine/editor/material/MaterialSession.hpp` | final : sessions::IEditSession；由 SessionStore 独占；组合源、SessionState、唯一 EditHistory |
| lux::material::MaterialSource | 复用原 modules 中的定义 | 作者事实只有一份，不造第二个同名源再双向同步；来源 ID 与 NodeId/PinId 分清 |
| MaterialReadView / MaterialSnapshot | 同目录 `MaterialSnapshot.hpp` | 同步借用与冻结拥有值分开；不得从 const 外壳获得可写节点或共享可变对象图 |
| MaterialEditBatch / Receipt / 领域错误 | 同目录 `MaterialEdit.hpp`，必要的独立语义错误头 | 有序意图、完成事实、失败分开；一次有效 batch 一次历史提交 |
| MaterialGraphEdit / MaterialValueEdit | `tools/material/model/src/edits/` | 复用 EditOperation/PreparedEdit；逆操作拥有必要节点/值和代码寿命，不引用 GraphElement |
| MaterialSessionAccess | 同目录 `MaterialSessionAccess.hpp` | 复用现有 typed key/access 的窄能力，不开放 engine/project/panes |
| PreparedMaterialReload | `tools/material/model/src/PreparedMaterialReload.hpp` | 私有完整候选；不是安装 SDK 中的任意 replaceSource 后门 |

必须实现 R1 已有的私有标量内容戳查询与 prepareClose。`currentContent() noexcept` 从现有身份和真实 History 得出，不调用完整 describe，不存第二份 current。源及节点必须长于借用它们的历史 memento，代码 lease 必须覆盖相关虚调用、allocator/deleter 和最后析构。

## 6. 逐项迁出与暂留

| 旧职责 | P03 处理 | 不应混入新会话的内容 |
|---|---|---|
| source_、history_、作者图及其布局 | 提取为真实 MaterialSource + SessionState + History 的组合 | 不保留一个 SceneSession 作为材质内部对象 |
| canEdit / busy_ | 使用 SessionState 的唯一准入 | 不新增 material_busy/compiling_blocks_all 等并行状态 |
| rename、常量、shading/render state | 具体值编辑，验证实际类型和合法范围；NO_CHANGE 不新增历史 | 不把编译产物是否准备好作为普通编辑前提 |
| texture/parameter slots | 带内容基线的声明编辑，引用及逆操作整体准备 | 不只替换数组后再修引用 |
| insert/replace/remove nodes | 统一候选和所有权规则，保留节点/引脚身份 | 失败不能使 live 图接管半成品，不能泄漏/双删 |
| connect/disconnect | 沿用现有方向、类型、重复和环规则 | 不引入全局动态命令系统替代领域方法 |
| moveNode/moveNodes | 若已序列化，则属于可撤销作者布局 | pan/zoom、悬停、选择仍是视图状态 |
| GraphDelta/GraphEditOperation/TValueEdit | 提取真正可复用的纯逻辑，删除原纯算法体 | 旧图控件不是 memento 的数据源 |
| compilation_/requestCompile/requestPublish | 原侧保留并登记 P07 去向 | 不属于 MaterialSession 或 IEditSession |
| preview_/previewCamera/maintainPreview | 原侧按 P07/P10 去向暂留 | 不因材质需要显示效果就创建 Runtime |
| save_/close_request_/候选 IO/content_/EditorContext | 按 P05/P12/P10 既有安排暂留 | 不引入新模型；原 LegacyPersistenceState 不允许反向使用 |

旧 MaterialEditor、Impl、MaterialAssets 不能整目录提前删除。旧产品尚未切换，必要适配仍需工作；可共享的纯算法则不因旧窗口存在而保留重复实现。每项记录“已迁出并删除原体 / 仍为旧接线适配 / 未迁移及后续责任”，不得笼统声明整个旧 MaterialEditor 已迁移。

新模型不得把所有函数转发到旧 Impl。旧侧要复用新纯机制，只能形成旧→新单向依赖；不能反向形成环。

## 7. 将 P02 教训直接落实到 P03

### 7.1 批次顺序与对象存在周期

节点删除、重建或替换，必须结束旧节点对应的临时字段和连接编辑状态。后续值修改以此时存在的节点类型及载荷为准。不能在批次末尾把旧节点 scratch 回写进新节点；不能只验证返回成功。

应纳入 Material 真实行为测试：先改常量、再替换/重建节点、再改新节点字段；检查没有明确修改的新载荷字段也保持正确。一次 Undo 恢复整个批次之前，Redo 恢复之后。不能接受的顺序在提交前整体失败，图、历史、观察版本和保存基线不变。

### 7.2 读取回调与动态节点寿命

Material 的 capture、节点 clone/encode 或其他实际会执行扩展回调的拥有型读取，使用已存在的 `EditGate::withRead()` 保护全程，包括中间值析构与异常展开。不新建锁系统或平行标志。

回调期间重入修改、Undo/Redo、候选采用、关闭/绑定准备必须拒绝或由已有上层显式安排；领域模型不暗中排队。不把普通无回调标量查询升级为长寿命共享锁。

### 7.3 失败后的所有权要与 API 签名相符

插入/替换成功才把节点纳入 live 图。失败时节点仍由调用者或准备候选负责销毁/重试，具体由选定接口契约说明并验证。不能使用按值消耗输入的接口，却承诺调用者原 unique_ptr 从未改变。

节点、快照、历史 memento 和代码保活需按真实对象图验证。`shared_ptr<const T>` 或 `graph.clone()` 的名字不证明深度冻结；先查实现，再通过继续编辑与晚析构证明。

### 7.4 目录减少不能靠扩大依赖

material_model 独立验证公开头及链接闭包。禁止 UI、旧 Context/Editor、Runtime、preview、compiler、ProjectStorage 实现与私有桥；实际需要的纯图、描述、数学和身份依赖如实登记。

P02 的 scene_model 规则不会自动替 material_model 完成全部验证。复用现有架构检查器，增加准确目标和真实负例，不另建一套检查框架，也不因为 PRIVATE/static/imported 而跳过传递闭包。

## 8. P03 内部实施顺序

**A — 源与依赖。** 阅读真实 MaterialSource、图结构、codec、NodeId/PinId 与原编辑算法，确认持久布局及运行派生态。建立最小 material_model 和实际 CPU 创建/查询。

**B — 编辑与历史。** 提取字段及图变更准备，统一准入和单次提交；验证节点/引脚引用及失败原子性。保留现有 History 算法，不引入图专用 UndoStack。

**C — 冻结与重载。** 审计动态节点的 clone/encode，覆盖读取回调；完成冻结快照与私有候选采用。源/新 History/checkpoint 配套，失败保留旧会话，成功整体换代。没有文件 IO、对话框和完整 Save As。

**D — 旧消费者收敛。** 新旧实际调用共享已提取纯机制，删除重复算法体；旧 UI、预览、编译及持久化接线按期限暂留。同步编译、安装和当前账本。

**E — 验收与停止。** 显式 P03 门禁、原 X03 与相关回归、独立安装的 CPU Material 使用证明；最终实现与证据分开提交，推送后停在 P03。

## 9. 必测与范围说明

| 场景 | 必须保留的观察点 |
|---|---|
| X03-01 | 无 GPU/窗口/编译器的真实材质节点、连接、槽声明编辑，Undo/Redo 和 capture |
| X03-02 | 节点插入/替换准备失败：原图与历史不变，输入/候选/live 所有者明确，无泄漏或双删 |
| X03-03 | 持久节点布局进入快照与历史；视图变换不污染作者源。P03 不为此提前创建 P10 产品窗口 |
| X03-04 | 动态节点替换后旧快照不变；代码与 allocator/deleter 生存期覆盖最后使用 |
| 继承的组合语义 | 节点替换与字段交错、重复字段与非法顺序、NO_CHANGE、预算失败都检查完整图和历史，不只看返回值 |
| 继承的读取契约 | 有回调的读取期间重入编辑及关闭/重载受阻；错误/异常退出恢复；后续正常操作成功 |
| 前置回归 | P01/R1 会话与关闭、P02 原五场景和本轮八项混合/读取回归、受影响旧产品功能继续通过 |
| 模块与安装 | material_model 的直接/传递依赖负例、私有桥和 UI/Runtime/compiler 禁入、安装 SDK 独立消费者 |

仍按原 P03 对应 Q01/Q07/Q09/Q11/Q27 报告实际范围；不提前认领 P07/P10 的编译、预览和产品视图整合完成。上述组合及读取测试是既有契约在 Material 的落实，不要求建立任意交错的无限枚举测试平台。

51 是本轮归档数量，不是下一轮必须维持的总数。不能删除旧断言、静默跳过失败或把真实产品缺陷设成 WILL_FAIL 来维持数字。开始和最终验收都要记录实际阶段值，最终必须为 `LUX_EDITOR_MIGRATION_STAGE=P03`。

## 10. 交付、证据与禁止续行

交付完整实现 SHA、独立验收 SHA、当前迁移账本的冻结副本、新增/迁出/删除/暂留清单、测试与命令映射、可取得日志和校验和。唯一可变材料仍在 `.internal/editor-redesign/`，冻结材料到 `dev_log/P03/`。

历史证据的源码存在性与哈希按各自 implementation_sha 验证，不按当前目录反推旧提交。新依赖门禁和删除审计则针对当前树。不要重写原 P00/P01/P02 收据以迎合新布局。

修正父阶段的新发现只限真实前置缺陷，并写清影响及测试；不以“通用化”为名开启另一轮框架重设计。C01/C03/C04、索引 World 和大世界性能等原范围不扩大。

所有必测完成才报告 PASS；否则 PARTIAL/BLOCKED 并停止。正常推送实施分支，不 force-push，不修改 main，不自动进入 P04。检查工作区，保留任何用户未提交内容并说明它是否被本轮验收覆盖。

## 11. 可直接发送给实施方

> P02 R1 复审通过，允许进入 P03，仅执行 P03。以 e0f440067792dc661e99a4f861a4116eeab4d304 为已验收前置，核对实际分支、工作区及祖先关系，不 reset 用户修改、不修改 main。
>
> 按原 V4 P03 与本启动补充，实现独立 Material 作者图。复用 lux::material::MaterialSource、NodeId/PinId、唯一 History、SessionState、SessionStore、私有 currentContent 和 withRead；不包装旧 MaterialEditor::Impl，不把 SceneSession 泛化成万能文档。
>
> material/model 归 Material 功能目录；历史和会话只在 editor/editing/history、editor/editing/sessions，旧目录不得重建。保留现有 target/include/包名，不扩大根目录碎片，不为每个小类型新增库。
>
> 明确节点/组件式存在周期的批次顺序，避免旧临时值覆盖替换节点；有扩展回调的读取复用唯一 gate。纯业务算法迁出后删除旧体，必要旧 UI/预览/编译/IO 适配按限定消费者与 P12 期限暂留，新模块禁止反向依赖。
>
> 执行 X03-01～04、相关 Q、继承的混合批次/读取契约、P01/P02 回归、依赖负例及独立安装消费者。最终显式 P03 门禁，保留真实失败证据；实现与验收分别提交，推送后停在 P03 等待复审，不进入 P04。

## 12. 固定审阅来源

以下 GitHub 链接均固定到本次复审 HEAD；它们是本次结论的依据，不代表全仓库逐行复审。

- [提交链与分支](https://github.com/LUX-YU/lux-engine/commits/e0f440067792dc661e99a4f861a4116eeab4d304)
- [P02-R1 验收说明](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/README.md)
- [SceneObjectEdit.cpp](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/editor/tools/scene/model/src/edits/SceneObjectEdit.cpp)
- [混合批次回归](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/editor/tools/scene/model/test/scene_session.cpp)
- [codec 读取回归](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/editor/tools/scene/model/test/plugin_snapshot.cpp)
- [迁移后的 SessionState / EditGate](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/editor/editing/sessions/include/lux/engine/editor/sessions/SessionState.hpp)
- [目录及闭包记录](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/evidence/structure.json)
- [架构负例源码](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/editor/tests/architecture/test_editor_boundaries.py)
- [架构负例日志](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/logs/boundaries.log)
- [51 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/logs/ctest.log)
- [读取重入修复前](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/negative/read-5.log)
- [读取重入修复后](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/logs/model-read-5.log)
- [冻结证据验证入口](https://github.com/LUX-YU/lux-engine/blob/e0f440067792dc661e99a4f861a4116eeab4d304/dev_log/P02-R1/check_receipt.py)

原 P03 与上一轮复审指令来自本会话提供的 V4 文件和 R1 补充，不用外部资料补写其既有要求。第 7 节等启动约束是本轮明确给出的继承/实施解释，不冒充仓库已经实现的 Material 功能。
