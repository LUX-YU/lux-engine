# P01 R1 复审结论与 P02 启动指令

**日期：2026-09-28**  
**项目：LUX-YU/lux-engine**  
**分支：codex/editor-redesign-v4**  
**本次读取的分支 HEAD：`3bc49e44646df30ea73990864658a9121789757a`**  
**R1 实现提交：`c3652237642085bf309620ab2b5229a6d90a0508`**

## 0. 决定与审阅边界

**可以进入 P02，仅授权 P02，不自动进入 P03，不合并 main。**

本轮已核对分支 HEAD、R1 实现的关键代码、测试源码、归档 CTest 记录、证据迁移测试结果及验收脚本。未发现需要再做一轮 P01 补正的前置阻塞项。

这是一项源码与已提交证据复审后的放行决定。本轮没有独立重跑引擎全量构建、37 项 CTest、安装消费者或 GPU 测试，也没有逐份独立复算全部归档哈希。Windows 隔离 Git/证据目录测试不等同于 Linux/Android 引擎资格认证。实施方的运行记录与本轮审阅范围必须分开表述。

本文件是 V4 `phases/P02_scene_authoring.md` 的启动补充，不是替代架构。继续使用原 V4 的类型索引、迁移账本与测试矩阵；只将已经实施的 R1 接口补正、实际起点和交接约定带入 P02。不新增另一套可独立变化的施工账本。

## 1. R1 三项复审

| 项目 | 读取到的修正 | 复审结论 |
| --- | --- | --- |
| R01 关闭异常契约 | `IEditSession` 增加私有 `currentContent() const noexcept`；`SessionStore::close()` 在 `CallbackScope` 内验证该内容戳，再消费 permit 和回收；不再调用完整 `describe()` | 原 owner、代际及内容戳验证保留，修正没有靠放宽契约实现 |
| R02 基础依赖门禁 | 三基础目标有直接依赖与闭包允许清单；检查 imported 目标后续依赖、未知链接项及源码 include；真实 CMake 负例核对规则与完整链路，并要求修复同一夹具后成功 | 上轮列出的五项漏报已有定向覆盖；不将其描述为任意 CMake/预处理语义的完整证明 |
| R03 证据路径 | 原 inventory 归档；验证器使用 `archive_path/archive_log` 与 `--evidence-root`；保留 producer 路径作来源信息；缺失、篡改和恢复有测试 | 可搬运证据和来源记录已分离；历史测试与结果没有被改写成新一次运行 |

会话测试保留了错误类型、错误 Store、错误线程、旧 key、permit 移动/消费、槽位复用与析构顺序。新增测试还覆盖：完整描述查询抛异常后的回调深度恢复；关闭时不调用 describe；故障注入使历史改变后旧许可不能关闭对象。测试中绕过 gate 修改内容，是用于检验最终校验的显式故障注入，不能作为业务用法抄入 SceneSession。

归档 `logs/ctest.log` 记录 37/37；新增两项关闭测试，原 35 项保留。验收记录列出 SDK 重新安装后五组、六项消费者检查。三个旧缺陷仍按原责任保留失败，不阻塞 P02。

## 2. 执行起点与材料

1. 从当前实施分支继续，读取实际 HEAD、工作树和祖先关系。`3bc49e44646df30ea73990864658a9121789757a` 必须是 P02 的已验收前置祖先；若已有后续提交，先解释差异，不 reset 到旧 SHA，不覆盖用户修改。
2. 读取 `dev_log/P00/`、`dev_log/P01/`、`dev_log/P01-R1/` 的记录，以及唯一可变施工材料 `.internal/editor-redesign/`。原 V4 的 `docs/editor-redesign/` 示例路径已由 P00 的实际交接约定替代，不再创建第二份账本。
3. 以 V4 P02 文档及本文件为本轮指令；将 `IEditSession::currentContent()` 的 R1 决议同步到实际类型记录。文档中的 `SessionAccess<T>` 与代码的 `TSessionAccess<T>` 等已有命名以实码为准，不另造一组同义类型。
4. 保持原验收快照不变。P02 更新施工账本，阶段结束单独冻结到 `dev_log/P02/`，实现提交与证据提交分离。
5. 开始时可复核 R1 证据与关键回归；不是要求重复实施 P01。发现前置真实缺陷再报告，不把未来阶段的功能补入 P01。

## 3. P02 的唯一交付目标

**交付一个可以脱离 UI、SceneRuntime、窗口与 GPU 运行的真实 Scene 作者模型。**

使用真实小场景与 CPU 数据，完成创建、查询、结构及字段编辑、撤销重做、冻结快照、失败回滚和有界变更查询。不是只实现 FakeSession，不是重命名 `SceneEditor::Impl`，也不是把旧编辑器隐藏在新的 PImpl 中。

本阶段不切换产品窗口到新模型，不实现保存 IO、运行预览、投影、布局或整个 Open/Close 应用用例。模型的候选重载准备属于本阶段的领域不变量；异步读文件和应用层重载审阅属于后续阶段。

## 4. 必须建立或复用的类型与文件

| 类型 | 文件/位置 | 所有权与契约 |
| --- | --- | --- |
| `SceneSource` | `editor/tools/scene/model/include/lux/engine/editor/scene/SceneSource.hpp` | 唯一可写作者内容，由 SceneSession 组合；不是运行 Registry 的包装 |
| `SceneSession final : sessions::IEditSession` | 同目录 `SceneSession.hpp` | Store 独占会话；会话组合 SessionState、SceneSource、唯一 EditHistory |
| `SceneReadView / SceneSnapshot / SceneChangeSet` | 同目录 `SceneSnapshot.hpp` | 分别表达同步只读借用、冻结拥有值、有界增量；不得混为一个可随意共享修改的对象 |
| `SceneEditBatch / SceneEditReceipt / 领域错误` | 同目录 `SceneEdit.hpp` | 批次意图、完成事实和失败分开；一个批次一次历史提交 |
| `SceneSessionAccess` | 同目录 `SceneSessionAccess.hpp` | 必要时窄别名或适配既有 typed access；无全局服务获取入口 |
| `SceneObjectRef / SceneObjectLocator` | 同目录 `SceneObjectRef.hpp` | 复用 WorldObjectId；跨会话/历史的失效信息准确，不造平行持久对象身份 |
| `SceneObjectEdit / SceneFieldEdit / SceneConfigurationEdit` | `model/src/edits/` | 复用 EditOperation / PreparedEdit 的协议继承；memento 拥有必要数据，不借 UI 临时变量 |
| `PreparedSceneReload` | `model/src/PreparedSceneReload.hpp` | 私有完整候选；源、历史、基线和索引配套，失败保留旧状态 |

小值可在相应语义头中归组。不要为追求文件数量把每个枚举拆为单独库；也不要重新建立全局 Types.hpp、ServiceRegistry 或万能 DocumentManager。

### 4.1 必须继承 R1 的关闭接口

SceneSession 除 `describe()` 外，必须实现：

```cpp
private:
    ContentStamp currentContent() const noexcept override;
    SessionResult<ClosePermit> prepareClose(ContentStamp expected) noexcept override;
```

以上名称使用实际命名空间限定；示例只表达接口方向，不是完整可编译头。

`currentContent()` 从现有 SessionId 与历史当前 StateId 得出。不得调用完整 describe、复制字符串、发通知、改 gate、等待任务或执行 IO；不得再保存一份需要手工同步的 `current_`。应保证调用条件下读取当前历史状态有效，而不是对可能失败的查询无条件解引用。

公开 describe 仍然是可以分配的拥有型描述；关闭验证不依赖它。当前接口的公共/私有划分保持，不把 `currentContent` 改成任意外部写状态后门。

### 4.2 顺序与唯一事实来源

会话中源对象的寿命必须长于历史 memento，快照及插件值的 deleter 完成前代码 lease 必须仍有效。保留 SessionState 的唯一编辑准入，不新增 `editing_busy` 与 gate 并行维护。

WorldObjectId 直接复用 `engine/domain/world/identity/include/lux/engine/world/WorldObjectId.hpp`。HistoryId/StateId/SessionId 是不同事实，不通过当前 AssetId 或裸 ECS Entity 冒充作者对象地址。

## 5. 内部实施顺序

### P02-A：实际源存储与纯域依赖

先阅读 ScenePackage、SceneContent、WorldMaterializer 和实际 codec。逐字段确认作者态与派生态，记录依据。系统配置、分区、世界和仿真配置的持久内容不能因为看起来像“运行数据”而丢弃；编辑相机、选择、高亮、工作平面和视口输出不能被误放进作者文件。

建立 `scene_model` target、源存储、创建候选和只读访问。最早的行为测试必须使用真实 CPU 场景数据，不创建 Runtime 或 UI Root。

### P02-B：原子编辑与历史闭合

实现结构编辑、字段编辑、配置编辑、模型插入和相机参数转作者对象等既定领域入口。模型插入接收已加载的 ModelAsset；IO 不在模型内。

prepare 完成全批次校验、预算预留和逆操作准备；commit 不再执行可失败的分配/解析/外部操作。源与历史状态不向观察者暴露半次更新。第三个对象创建失败、父环、schema 缺失及预算不足均应保持整个批次失败，不以逐项成功冒充原子批次。

undo/redo 走同一 EditGate 与唯一 History。不引入一个模型侧 UndoStack。所有借用、memento 和 callback 的寿命按真实值图检查。

### P02-C：冻结快照与增量变化

实现 `capture(SnapshotBudget)`，冻结已提交状态，不捕获临时交互预览。外层 `shared_ptr<const T>` 不足以证明插件节点深度冻结；用具体的深复制、不可变节点或写时复制机制证明，并记录其成本。

实现 `changesSince` 的有界记录。记录被裁剪、查询版本失效或历史整体换代时返回明确的 ResetRequired。发布观察版本与保存基线分离，禁止用通知序号判定 dirty。

准备领域重载候选及原子采用的必要内部机制，但不实现磁盘读取和用户对话框。不要为通过换代测试暴露任意 replaceSource/markClean 公共后门。

### P02-D：旧算法迁移与双实现清理

逐项处理旧 ParentEdit/ObjectEdit、makeParentEdit/makeObjectEdit、createObject/eraseObjects/reparent/createEntitiesFromModel、字段及配置编辑中的纯业务逻辑。

可复用的业务算法只留一个实现，新旧调用通过受限适配使用它。仍然依赖旧 Registry 的绑定/投影适配可以有期限地留在旧侧，但不允许新作者模型 include 它，也不允许它继续复制已经提取的纯算法。

对每项旧符号记录实际动作：移到哪里、谁仍在调用、是否只剩适配、何时删除。禁止把“旧窗口还没切换”当作新旧两套业务长期并存的理由。

不得整目录删除旧 SceneEditor；旧产品窗口直到后续切换仍需工作。真正提取掉的纯算法则应同步删除旧体，不留 `.old`、空壳、注释副本或 `#if 0`。

### P02-E：完整测试与安装验证

运行 X02-01～05、相应 Q 场景、R1 回归及受影响旧产品测试。增加独立安装的真实 SceneSession 使用证明；公开头和链接不能暗中依赖源码私有目录。

阶段末显式使用 `LUX_EDITOR_MIGRATION_STAGE=P02`。不能继续用 P01 的期限检查结果声称 P02 通过。测试数量只用于索引，不替代行为、删除范围和依赖证据。

## 6. 模型的依赖门禁必须同步建立

R1 加强的是三个基础 target，不表示未来 scene_model 的全部依赖已自动获得相同保护。P02 应为新模型建立明确的直接和传递检查。

允许复用 history/sessions、world identity、资产/场景描述、数学、schema 与其他确实纯 CPU 的依赖。允许项按真实 target 与安装闭包登记，不凭名字推断。

模型禁止依赖：旧 EditorContext、SceneEditor/Impl、Pane/Root/UI、SceneRuntime/scene_composition、GPU/窗口、ProjectStorage 实现及 LegacyPersistenceState 桥。PRIVATE、static LINK_ONLY、imported 中间跳转都不能绕过。

至少用真实 CMake 负例证明：直接依赖运行时、经中间 target 依赖 UI 或旧 Context、源码含入旧桥都能被正确规则拒绝；修复同一夹具后可通过。不能因缺包/语法错误而算作门禁有效。

不需要借此升级为全仓库通用架构平台；保护实际新建的模型边界即可。纯域 target 若原来捎带 Runtime，需要在实际依赖处提取真正的纯子集，不是放宽模型禁边。

## 7. 必须保留的五项 P02 验收语义

| 编号 | 必须观察到的结果 |
| --- | --- |
| X02-01 | 真实 CPU 场景、无 Root/Runtime/GPU，结构/字段编辑、undo 和 capture 可用，安装闭包干净 |
| X02-02 | 多对象第三处故障、父环等失败后，全部源对象与历史游标不变，无部分提交 |
| X02-03 | 捕获后修改插件节点，旧快照不变；释放加载方句柄后，节点/deleter 所需代码仍存活 |
| X02-04 | 作者寻址不依赖当前文件根 AssetId 或 Runtime Entity；不提前实现整个 Save As 来验此项 |
| X02-05 | 增量记录裁剪与 HistoryId 换代都返回 ResetRequired，不伪称拥有完整变化 |

相关 V3 场景：Q01、Q06、Q09、Q11、Q22、Q26。按本阶段真实责任报告覆盖范围，不把尚未实施的 UI/运行整合标为完成。

P01/R1 的关闭、checkpoint、许可、候选、代码寿命及依赖负例继续受保护。新增 SceneSession 必须使用 R1 的私有内容戳契约，不把测试中的故障注入用法当作模型正常接口。

## 8. 允许暂留与禁止扩张

允许限期暂留：旧产品的 SceneEditor/Impl、旧 Registry 绑定适配、与其配套的 LegacyPersistenceState。它们遵守原账本期限，最迟 P12 删除，不进入新模型 SDK。

不属于 P02：材质/Flow 作者模型整体迁移、保存 IO、运行会话和 SceneRuntime 重写、渲染投影、高亮、交互拖拽、布局/菜单/退出应用流程、全局插件系统和 Lua 性能重构。

C01 继续归 P09/P12；C03 继续归 P11；C04 继续归 P12。保留失败与历史日志，不改断言、不包装为通过，也不借本轮顺手修复。若发现 P02 新模型自身出现同类生命周期缺陷，必须修新模型，不能借这些旧编号延期。

## 9. 阶段交付与停点

交付源代码、实现完整 SHA、独立验收提交、`dev_log/P02/` 收据与固定快照、更新后的唯一施工账本、真实命令与可取得日志、文件和必要证据哈希。

报告必须分别列出：

- 新模型实际完成的行为、唯一 owner、身份和保存基线的责任。
- 旧纯算法的迁出位置与原实现删除情况；旧适配的消费者和明确删除期限。
- X02/Q/P01-R1 回归、架构负例、完整产品及安装消费者结果；未运行项目不能算通过。
- 原三缺陷仍失败以及其他实际限制。

所有必须门槛通过才报 PASS；否则 PARTIAL/BLOCKED，停在 P02。实现与验收提交分开，正常推送实施分支，不 force-push、不修改 main、不自动开始 P03。

**本次正式推进目标：从已经通过复审的历史与会话基础，进入真正独立的 Scene 作者模型。不是继续增加 Editor 外壳。**

## 固定审阅来源

以下均固定到本轮读取的提交，不随分支后续变化漂移：

- [R1 验收记录](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/dev_log/P01-R1/README.md)
- [IEditSession](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/editor/sessions/include/lux/engine/editor/sessions/IEditSession.hpp)
- [SessionStore](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/editor/sessions/src/SessionStore.cpp)
- [会话回归测试](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/editor/sessions/test/sessions.cpp)
- [架构检查器](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/editor/tests/architecture/check_editor_boundaries.py)
- [真实 CMake 负例](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/editor/tests/architecture/test_editor_boundaries.py)
- [证据验证器](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/dev_log/P01/verify.py)
- [证据迁移与损坏测试](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/editor/tests/architecture/test_p01_evidence.py)
- [37 项 CTest 归档](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/dev_log/P01-R1/logs/ctest.log)
- [R1 收据验证入口](https://github.com/LUX-YU/lux-engine/blob/3bc49e44646df30ea73990864658a9121789757a/dev_log/P01-R1/check_receipt.py)
