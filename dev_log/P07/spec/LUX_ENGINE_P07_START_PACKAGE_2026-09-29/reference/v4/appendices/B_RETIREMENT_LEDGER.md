# 逐成员、函数与文件迁移账本

**本表是固定基准的人工调查种子，不是完整仓库 AST 扫描结果。** P00 必须在实际源树中补齐每个声明/定义/调用点、未列私有字段、生成输入和安装导出；任何未分类项阻塞基线门槛。不得把“已看见这些符号”写成“仓库再没有其他消费者”。

每条有两个时间：首次实现接替职责的阶段，以及旧符号必须消失的阶段。P02–P11 新模型可在非产品测试入口验证；旧产品在 P12 前允许暂存，但不得与新产品共享同一工作副本并行写入。旧 wrapper 不得偷藏作者内容来规避 Store 唯一所有权。P12 完成唯一入口切换与清零，P13 不再接纳“以后删除”的延期。

`D` 条目按**限定 owner**匹配。`source`、`update`、`valid`、`Content` 等不构成全仓库禁词；要删除的是原 owner 下的职责与调用协议，不是把所有同名算法误删。成员旁的新名称是责任归属，不保证一对一字段改名：若旧状态由正确关系自然推导，就直接删除，而不是迁出一个多余 bool。

“P12 删除”允许在消费者已经迁完时提前完成，但不得在行为尚无替代时删除功能。没有适配者时需补真正实现或报告 BLOCKED，不允许删测试/默认返回成功。

## 1. 逐限定符迁移

### SceneEditor::Impl

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0001 | `SceneEditor::Impl::source` | 成员 | P02 / P12 | SceneSession / SceneSource / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0002 | `SceneEditor::Impl::content` | 成员 | P02 / P12 | SceneSession / SceneSource / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0003 | `SceneEditor::Impl::history` | 成员 | P02 / P12 | SceneSession / SceneSource / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0004 | `SceneEditor::Impl::scene_editing` | 成员 | P02 / P12 | SceneSession / SceneSource / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0005 | `SceneEditor::Impl::makeParentEdit` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0006 | `SceneEditor::Impl::makeObjectEdit` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0007 | `SceneEditor::Impl::component` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0008 | `SceneEditor::Impl::writeTarget` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0009 | `SceneEditor::Impl::checkStructure` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0010 | `SceneEditor::Impl::supportsObjectSpace` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0011 | `SceneEditor::Impl::supportsHierarchy` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0012 | `SceneEditor::Impl::createObject` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0013 | `SceneEditor::Impl::eraseObjects` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0014 | `SceneEditor::Impl::reparent` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0015 | `SceneEditor::Impl::createEntitiesFromModel` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0016 | `SceneEditor::Impl::partitionCount` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0017 | `SceneEditor::Impl::executeContent` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0018 | `SceneEditor::Impl::editing` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0019 | `SceneEditor::Impl::componentVersion` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0020 | `SceneEditor::Impl::historyId` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0021 | `SceneEditor::Impl::historyView` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0022 | `SceneEditor::Impl::undo` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0023 | `SceneEditor::Impl::redo` | 函数 | P02 / P12 | SceneSession 具体编辑与只读接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0024 | `SceneEditor::Impl::editing_busy` | 成员 | P02 / P12 | SessionState::EditGate；投影和 UI 分别诊断。busy 不是可复制 EditingGuard；通用 failure 拆为具体结果/诊断，不另加万能 FailureState。 |
| D0025 | `SceneEditor::Impl::finishing_interaction` | 成员 | P02 / P12 | SessionState::EditGate；投影和 UI 分别诊断。busy 不是可复制 EditingGuard；通用 failure 拆为具体结果/诊断，不另加万能 FailureState。 |
| D0026 | `SceneEditor::Impl::failure` | 成员 | P02 / P12 | SessionState::EditGate；投影和 UI 分别诊断。busy 不是可复制 EditingGuard；通用 failure 拆为具体结果/诊断，不另加万能 FailureState。 |
| D0027 | `SceneEditor::Impl::structure_revision` | 成员 | P02 / P12 | 结构通知 ObservationVersion 与具体变更集。保留独立内容 StateId 与观察版本；不把单调通知值当持久状态身份。 |
| D0028 | `SceneEditor::Impl::observed_history` | 成员 | P02 / P12 | 结构通知 ObservationVersion 与具体变更集。保留独立内容 StateId 与观察版本；不把单调通知值当持久状态身份。 |
| D0029 | `SceneEditor::Impl::save` | 成员 | P05 / P12 | SceneSaveSource / SaveService / SceneSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0030 | `SceneEditor::Impl::next_save` | 成员 | P05 / P12 | SceneSaveSource / SaveService / SceneSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0031 | `SceneEditor::Impl::copied_source_` | 成员 | P05 / P12 | SceneSaveSource / SaveService / SceneSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0032 | `SceneEditor::Impl::change_save_` | 成员 | P05 / P12 | SceneSaveSource / SaveService / SceneSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0033 | `SceneEditor::Impl::captureSource` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0034 | `SceneEditor::Impl::requestSave` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0035 | `SceneEditor::Impl::requestSaveAs` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0036 | `SceneEditor::Impl::saveRequests` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0037 | `SceneEditor::Impl::saveStatus` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0038 | `SceneEditor::Impl::retrySave` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0039 | `SceneEditor::Impl::abandonSave` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0040 | `SceneEditor::Impl::acknowledgeSave` | 函数 | P05 / P12 | SceneSaveSource 与具体保存/另存操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0045 | `SceneEditor::Impl::run_scene` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0046 | `SceneEditor::Impl::run_status` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0047 | `SceneEditor::Impl::next_run` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0048 | `SceneEditor::Impl::run_delta` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0049 | `SceneEditor::Impl::step_baseline_` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0050 | `SceneEditor::Impl::single_step` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0051 | `SceneEditor::Impl::run_stop` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0052 | `SceneEditor::Impl::run_prepared_` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0053 | `SceneEditor::Impl::run_preparation` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0054 | `SceneEditor::Impl::run_source` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0055 | `SceneEditor::Impl::run_assets` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0056 | `SceneEditor::Impl::run_history` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0057 | `SceneEditor::Impl::run_editing` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0058 | `SceneEditor::Impl::run_connections` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0059 | `SceneEditor::Impl::run_receipt` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0060 | `SceneEditor::Impl::run_page_size` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0061 | `SceneEditor::Impl::run_catalog_changed` | 成员 | P06 / P12 | RunSession / RunStore / RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0062 | `SceneEditor::Impl::runtime_` | 成员 | P06 / P12 | SceneInstanceLease / SceneRuntime 单一退休机制。runtime 可由具体 engine adapter 短借；旧多 optional+terminate 清理不可照搬。 |
| D0063 | `SceneEditor::Impl::scenes_guard_` | 成员 | P06 / P12 | SceneInstanceLease / SceneRuntime 单一退休机制。runtime 可由具体 engine adapter 短借；旧多 optional+terminate 清理不可照搬。 |
| D0064 | `SceneEditor::Impl::destroyScene` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0065 | `SceneEditor::Impl::safe` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0066 | `SceneEditor::Impl::registry` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0067 | `SceneEditor::Impl::readRegistry` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0068 | `SceneEditor::Impl::progress` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0069 | `SceneEditor::Impl::runStructureChanged` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0070 | `SceneEditor::Impl::observeRun` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0071 | `SceneEditor::Impl::runSettled` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0072 | `SceneEditor::Impl::play` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0073 | `SceneEditor::Impl::pauseRun` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0074 | `SceneEditor::Impl::resumeRun` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0075 | `SceneEditor::Impl::stepRun` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0076 | `SceneEditor::Impl::stopRun` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0077 | `SceneEditor::Impl::runStatus` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0078 | `SceneEditor::Impl::runCoordinatePageSize` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0079 | `SceneEditor::Impl::observePlaybackRender` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0080 | `SceneEditor::Impl::adoptPlayback` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0081 | `SceneEditor::Impl::updatePlayback` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0082 | `SceneEditor::Impl::observePlayback` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0083 | `SceneEditor::Impl::beginPauseEditing` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0084 | `SceneEditor::Impl::failPlayback` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0085 | `SceneEditor::Impl::restorePlayback` | 函数 | P06 / P12 | SceneRuntime 受限短借与 RunController。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0086 | `SceneEditor::Impl::scene` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0087 | `SceneEditor::Impl::asset_source` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0088 | `SceneEditor::Impl::render_receipt` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0089 | `SceneEditor::Impl::viewport_system_` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0090 | `SceneEditor::Impl::resource_snapshot` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0091 | `SceneEditor::Impl::observed_resources` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0092 | `SceneEditor::Impl::resource_instance` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0093 | `SceneEditor::Impl::changed_assets` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0094 | `SceneEditor::Impl::assets_connection` | 成员 | P07 / P12 | SceneProjection / ScenePresentationHub / instance lease。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0095 | `SceneEditor::Impl::highlighted_selection` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0096 | `SceneEditor::Impl::highlighted_structure` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0097 | `SceneEditor::Impl::highlighted_feature` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0098 | `SceneEditor::Impl::highlighted_instance` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0099 | `SceneEditor::Impl::highlight_program` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0100 | `SceneEditor::Impl::highlight_pending` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0101 | `SceneEditor::Impl::work_plane_height` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0102 | `SceneEditor::Impl::work_plane_pending` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0103 | `SceneEditor::Impl::work_plane_program` | 成员 | P07 / P12 | 每视口 HighlightRenderer / WorkPlaneRenderer。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0104 | `SceneEditor::Impl::renderFor` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0105 | `SceneEditor::Impl::inspectedRender` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0106 | `SceneEditor::Impl::submit` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0107 | `SceneEditor::Impl::updateWorkPlane` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0108 | `SceneEditor::Impl::updateHighlight` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0109 | `SceneEditor::Impl::resources` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0110 | `SceneEditor::Impl::retryResource` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0111 | `SceneEditor::Impl::renderScene` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0112 | `SceneEditor::Impl::coordinatePageSize` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0113 | `SceneEditor::Impl::selectRenderSystem` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0114 | `SceneEditor::Impl::selectedRenderSystem` | 函数 | P07 / P12 | 投影/视口接口；作者/运行绑定显式。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0115 | `SceneEditor::Impl::selection_` | 成员 | P08 / P12 | SceneInteractionState / SelectionContext / GestureTransaction。相机/选择属于对应视图或明确共享的 interaction context；作者保存不包含编辑器视图相机。 |
| D0116 | `SceneEditor::Impl::run_selection` | 成员 | P08 / P12 | SceneInteractionState / SelectionContext / GestureTransaction。相机/选择属于对应视图或明确共享的 interaction context；作者保存不包含编辑器视图相机。 |
| D0117 | `SceneEditor::Impl::editor_camera` | 成员 | P08 / P12 | SceneInteractionState / SelectionContext / GestureTransaction。相机/选择属于对应视图或明确共享的 interaction context；作者保存不包含编辑器视图相机。 |
| D0118 | `SceneEditor::Impl::inspectedScene` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0119 | `SceneEditor::Impl::inspectedSelection` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0120 | `SceneEditor::Impl::resolve` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0121 | `SceneEditor::Impl::inspectedHistory` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0122 | `SceneEditor::Impl::selection` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0123 | `SceneEditor::Impl::select` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0124 | `SceneEditor::Impl::instance` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0125 | `SceneEditor::Impl::raycastNearest` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0126 | `SceneEditor::Impl::viewportCamera` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0127 | `SceneEditor::Impl::setWorkPlaneHeight` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0128 | `SceneEditor::Impl::navigateCamera` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0129 | `SceneEditor::Impl::createCameraFromView` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0130 | `SceneEditor::Impl::fieldEditWritable` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0131 | `SceneEditor::Impl::fieldEdited` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0132 | `SceneEditor::Impl::finishFieldEdits` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0133 | `SceneEditor::Impl::finishFieldEdit` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0134 | `SceneEditor::Impl::finishEditing` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0135 | `SceneEditor::Impl::finishContentEditing` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0136 | `SceneEditor::Impl::isEditingBusy` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0137 | `SceneEditor::Impl::checkEditAdmission` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0138 | `SceneEditor::Impl::writeRestriction` | 函数 | P08 / P12 | 受限交互/查询＋具体作者编辑。删除“有 run_scene 就自动切换编辑对象”的隐含重定向；命令使用显式绑定。 |
| D0139 | `SceneEditor::Impl::editor` | 成员 | P10 / P12 | SceneView / SceneCreationView / 独立检查工具视图。content 是作者内容，content_ 是 UI；分别迁移，不得按相似名字误删作者源。 |
| D0140 | `SceneEditor::Impl::creation_pane_` | 成员 | P10 / P12 | SceneView / SceneCreationView / 独立检查工具视图。content 是作者内容，content_ 是 UI；分别迁移，不得按相似名字误删作者源。 |
| D0141 | `SceneEditor::Impl::content_` | 成员 | P10 / P12 | SceneView / SceneCreationView / 独立检查工具视图。content 是作者内容，content_ 是 UI；分别迁移，不得按相似名字误删作者源。 |
| D0142 | `SceneEditor::Impl::inspector_` | 成员 | P10 / P12 | SceneView / SceneCreationView / 独立检查工具视图。content 是作者内容，content_ 是 UI；分别迁移，不得按相似名字误删作者源。 |
| D0143 | `SceneEditor::Impl::outliner_` | 成员 | P10 / P12 | SceneView / SceneCreationView / 独立检查工具视图。content 是作者内容，content_ 是 UI；分别迁移，不得按相似名字误删作者源。 |
| D0144 | `SceneEditor::Impl::resources_` | 成员 | P10 / P12 | SceneView / SceneCreationView / 独立检查工具视图。content 是作者内容，content_ 是 UI；分别迁移，不得按相似名字误删作者源。 |
| D0145 | `SceneEditor::Impl::createContent` | 函数 | P10 / P12 | SceneView 与应用驱动阶段。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0146 | `SceneEditor::Impl::syncInspector` | 函数 | P10 / P12 | SceneView 与应用驱动阶段。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0147 | `SceneEditor::Impl::update` | 函数 | P10 / P12 | SceneView 与应用驱动阶段。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0148 | `SceneEditor::Impl::event` | 函数 | P10 / P12 | SceneView 与应用驱动阶段。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0149 | `SceneEditor::Impl::candidate_scene_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0150 | `SceneEditor::Impl::candidate_viewport_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0151 | `SceneEditor::Impl::asset_status_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0152 | `SceneEditor::Impl::read_result_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0153 | `SceneEditor::Impl::reading_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0154 | `SceneEditor::Impl::candidate_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0155 | `SceneEditor::Impl::candidate_history_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0156 | `SceneEditor::Impl::candidate_assets_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0157 | `SceneEditor::Impl::candidate_content_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0158 | `SceneEditor::Impl::candidate_editing_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0159 | `SceneEditor::Impl::resume_after_change_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0160 | `SceneEditor::Impl::hide_requested_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0161 | `SceneEditor::Impl::replacing_` | 成员 | P12 / P12 | OpenAssetOperation / ReloadSessionOperation / PreparedSceneData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0162 | `SceneEditor::Impl::changeAsset` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0163 | `SceneEditor::Impl::reviewAsset` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0164 | `SceneEditor::Impl::startAssetChange` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0165 | `SceneEditor::Impl::applyAssetChange` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0166 | `SceneEditor::Impl::assetFailure` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0167 | `SceneEditor::Impl::prepareCandidate` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0168 | `SceneEditor::Impl::adoptCandidate` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0169 | `SceneEditor::Impl::reviewClose` | 函数 | P12 / P12 | 打开/重载/关闭的明确用例。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0170 | `SceneEditor::Impl::placement` | 成员 | P12 / P12 | ModelCreationOperation / SceneSession 编辑入口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0171 | `SceneEditor::Impl::next_placement` | 成员 | P12 / P12 | ModelCreationOperation / SceneSession 编辑入口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0172 | `SceneEditor::Impl::requestModelCreation` | 函数 | P12 / P12 | ModelCreationOperation ID/状态/终态消费。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0173 | `SceneEditor::Impl::modelCreationStatus` | 函数 | P12 / P12 | ModelCreationOperation ID/状态/终态消费。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0174 | `SceneEditor::Impl::retryModelCreation` | 函数 | P12 / P12 | ModelCreationOperation ID/状态/终态消费。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0175 | `SceneEditor::Impl::cancelModelCreation` | 函数 | P12 / P12 | ModelCreationOperation ID/状态/终态消费。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0176 | `SceneEditor::Impl::acknowledgeModelCreation` | 函数 | P12 / P12 | ModelCreationOperation ID/状态/终态消费。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0177 | `SceneEditor::Impl::completion_work_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0178 | `SceneEditor::Impl::completion_deferred_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0179 | `SceneEditor::Impl::close_request_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0180 | `SceneEditor::Impl::close_prepared_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0181 | `SceneEditor::Impl::close_decision_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0182 | `SceneEditor::Impl::close_connection_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0183 | `SceneEditor::Impl::editor_context_` | 成员 | P12 / P12 | 具体操作拥有完成结果；应用只推进 phase。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0184 | `SceneEditor::Impl::adoptCompletions` | 函数 | P12 / P12 | 具体操作完成采用 / 窄项目依赖 / Diagnostic。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0185 | `SceneEditor::Impl::applyChanges` | 函数 | P12 / P12 | 具体操作完成采用 / 窄项目依赖 / Diagnostic。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0186 | `SceneEditor::Impl::adoptAssetResults` | 函数 | P12 / P12 | 具体操作完成采用 / 窄项目依赖 / Diagnostic。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0187 | `SceneEditor::Impl::project` | 函数 | P12 / P12 | 具体操作完成采用 / 窄项目依赖 / Diagnostic。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0188 | `SceneEditor::Impl::diagnostic` | 函数 | P12 / P12 | 具体操作完成采用 / 窄项目依赖 / Diagnostic。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### SceneSaveCapture

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0041 | `SceneSaveCapture::capture` | 成员 | P05 / P12 | Owned FrozenSave + EncodedSceneClone。copied 可写侧信道删除；克隆 package 显式作为编码结果返回，不从 const 输入写出。 |
| D0042 | `SceneSaveCapture::copy_identity` | 成员 | P05 / P12 | Owned FrozenSave + EncodedSceneClone。copied 可写侧信道删除；克隆 package 显式作为编码结果返回，不从 const 输入写出。 |
| D0043 | `SceneSaveCapture::copied` | 成员 | P05 / P12 | Owned FrozenSave + EncodedSceneClone。copied 可写侧信道删除；克隆 package 显式作为编码结果返回，不从 const 输入写出。 |

### SceneEncoder

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0044 | `SceneEncoder::operator()` | 函数 | P05 / P12 | SceneEncodeJob::encode + 显式拥有结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### MaterialEditor::Impl

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/pinclude/lux/engine/editor/material/MaterialEditorImpl.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0189 | `MaterialEditor::Impl::source_` | 成员 | P03 / P12 | MaterialSession / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0190 | `MaterialEditor::Impl::history_` | 成员 | P03 / P12 | MaterialSession / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0191 | `MaterialEditor::Impl::busy_` | 成员 | P03 / P12 | EditGate；每操作独立错误与诊断。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0192 | `MaterialEditor::Impl::finishing_interaction_` | 成员 | P03 / P12 | EditGate；每操作独立错误与诊断。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0193 | `MaterialEditor::Impl::canEdit` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0194 | `MaterialEditor::Impl::editGraph` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0195 | `MaterialEditor::Impl::change` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0196 | `MaterialEditor::Impl::historyId` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0197 | `MaterialEditor::Impl::historyView` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0198 | `MaterialEditor::Impl::undo` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0199 | `MaterialEditor::Impl::redo` | 函数 | P03 / P12 | MaterialSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0200 | `MaterialEditor::Impl::save_` | 成员 | P05 / P12 | MaterialSaveSource / SaveService / MaterialSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0201 | `MaterialEditor::Impl::next_save_` | 成员 | P05 / P12 | MaterialSaveSource / SaveService / MaterialSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0202 | `MaterialEditor::Impl::saved_identity_` | 成员 | P05 / P12 | MaterialSaveSource / SaveService / MaterialSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0203 | `MaterialEditor::Impl::saved_history_` | 成员 | P05 / P12 | MaterialSaveSource / SaveService / MaterialSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0204 | `MaterialEditor::Impl::change_save_` | 成员 | P05 / P12 | MaterialSaveSource / SaveService / MaterialSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0205 | `MaterialEditor::Impl::requestSave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0206 | `MaterialEditor::Impl::requestSaveAs` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0207 | `MaterialEditor::Impl::saveRequests` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0208 | `MaterialEditor::Impl::saveStatus` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0209 | `MaterialEditor::Impl::retrySave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0210 | `MaterialEditor::Impl::abandonSave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0211 | `MaterialEditor::Impl::acknowledgeSave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0212 | `MaterialEditor::Impl::compilation_` | 成员 | P07 / P12 | Material 编译服务与稳定操作记录。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0213 | `MaterialEditor::Impl::compile_result_` | 成员 | P07 / P12 | Material 编译服务与稳定操作记录。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0214 | `MaterialEditor::Impl::compile_task_` | 成员 | P07 / P12 | Material 编译服务与稳定操作记录。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0215 | `MaterialEditor::Impl::requestCompile` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0216 | `MaterialEditor::Impl::requestPublish` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0217 | `MaterialEditor::Impl::compileStatus` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0218 | `MaterialEditor::Impl::compiled` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0219 | `MaterialEditor::Impl::editor_` | 成员 | P10 / P12 | MaterialView 与交互上下文。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0220 | `MaterialEditor::Impl::content_` | 成员 | P10 / P12 | MaterialView 与交互上下文。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0221 | `MaterialEditor::Impl::createContent` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0222 | `MaterialEditor::Impl::finishContentEditing` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0223 | `MaterialEditor::Impl::applyContentIntents` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0224 | `MaterialEditor::Impl::contentCommand` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0225 | `MaterialEditor::Impl::update` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0226 | `MaterialEditor::Impl::event` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0227 | `MaterialEditor::Impl::finishEditing` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0228 | `MaterialEditor::Impl::asset_status_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0229 | `MaterialEditor::Impl::read_result_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0230 | `MaterialEditor::Impl::reading_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0231 | `MaterialEditor::Impl::candidate_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0232 | `MaterialEditor::Impl::candidate_history_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0233 | `MaterialEditor::Impl::hide_requested_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0234 | `MaterialEditor::Impl::close_request_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0235 | `MaterialEditor::Impl::close_prepared_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0236 | `MaterialEditor::Impl::close_decision_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0237 | `MaterialEditor::Impl::close_connection_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0238 | `MaterialEditor::Impl::editor_context_` | 成员 | P12 / P12 | 窄依赖注入；操作自身完成邮箱。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0239 | `MaterialEditor::Impl::completion_work_` | 成员 | P12 / P12 | 窄依赖注入；操作自身完成邮箱。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0240 | `MaterialEditor::Impl::changeAsset` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0241 | `MaterialEditor::Impl::reviewAsset` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0242 | `MaterialEditor::Impl::startAssetChange` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0243 | `MaterialEditor::Impl::applyAssetChange` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0244 | `MaterialEditor::Impl::assetFailure` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0245 | `MaterialEditor::Impl::adoptCompletions` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0246 | `MaterialEditor::Impl::applyChanges` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0247 | `MaterialEditor::Impl::adoptAssetResults` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0248 | `MaterialEditor::Impl::project` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0309 | `MaterialEditor::Impl::source` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0310 | `MaterialEditor::Impl::rename` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0311 | `MaterialEditor::Impl::setConstant` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0312 | `MaterialEditor::Impl::setShadingModel` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0313 | `MaterialEditor::Impl::setRenderState` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0314 | `MaterialEditor::Impl::setTextureSlots` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0315 | `MaterialEditor::Impl::setParameterSlots` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0316 | `MaterialEditor::Impl::replaceNode` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0317 | `MaterialEditor::Impl::insertNode` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0318 | `MaterialEditor::Impl::removeNodes` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0319 | `MaterialEditor::Impl::connect` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0320 | `MaterialEditor::Impl::disconnect` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0321 | `MaterialEditor::Impl::moveNode` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0322 | `MaterialEditor::Impl::moveNodes` | 函数 | P03 / P12 | MaterialSession 具体可撤销编辑。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0323 | `MaterialEditor::Impl::preview_render_system_` | 成员 | P07 / P12 | MaterialPreviewStore / 每视图呈现。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0324 | `MaterialEditor::Impl::preview_` | 成员 | P07 / P12 | MaterialPreviewStore / 每视图呈现。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0325 | `MaterialEditor::Impl::preview_failure_` | 成员 | P07 / P12 | MaterialPreviewStore / 每视图呈现。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0326 | `MaterialEditor::Impl::resetPreview` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0327 | `MaterialEditor::Impl::createPreview` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0328 | `MaterialEditor::Impl::capturePreviewAssets` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0329 | `MaterialEditor::Impl::updatePreview` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0330 | `MaterialEditor::Impl::maintainPreview` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0331 | `MaterialEditor::Impl::previewCamera` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0332 | `MaterialEditor::Impl::previewInstance` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0333 | `MaterialEditor::Impl::previewStatus` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0334 | `MaterialEditor::Impl::navigatePreview` | 函数 | P07 / P12 | MaterialPreview 接口及特定视图相机。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0335 | `MaterialEditor::Impl::completion_deferred_` | 成员 | P12 / P12 | 具体完成采用，不再由窗口清空结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### FlowForgeEditor::Impl

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0249 | `FlowForgeEditor::Impl::source_` | 成员 | P04 / P12 | FlowSession / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0250 | `FlowForgeEditor::Impl::history_` | 成员 | P04 / P12 | FlowSession / SessionState / EditHistory。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0251 | `FlowForgeEditor::Impl::busy_` | 成员 | P04 / P12 | EditGate；每操作独立错误与诊断。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0252 | `FlowForgeEditor::Impl::finishing_interaction_` | 成员 | P04 / P12 | EditGate；每操作独立错误与诊断。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0253 | `FlowForgeEditor::Impl::canEdit` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0254 | `FlowForgeEditor::Impl::editGraph` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0255 | `FlowForgeEditor::Impl::change` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0256 | `FlowForgeEditor::Impl::historyId` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0257 | `FlowForgeEditor::Impl::historyView` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0258 | `FlowForgeEditor::Impl::undo` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0259 | `FlowForgeEditor::Impl::redo` | 函数 | P04 / P12 | FlowSession 的 edit/undo/redo 与查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0260 | `FlowForgeEditor::Impl::save_` | 成员 | P05 / P12 | FlowSaveSource / SaveService / FlowSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0261 | `FlowForgeEditor::Impl::next_save_` | 成员 | P05 / P12 | FlowSaveSource / SaveService / FlowSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0262 | `FlowForgeEditor::Impl::saved_identity_` | 成员 | P05 / P12 | FlowSaveSource / SaveService / FlowSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0263 | `FlowForgeEditor::Impl::saved_history_` | 成员 | P05 / P12 | FlowSaveSource / SaveService / FlowSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0264 | `FlowForgeEditor::Impl::change_save_` | 成员 | P05 / P12 | FlowSaveSource / SaveService / FlowSaveAsOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0265 | `FlowForgeEditor::Impl::requestSave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0266 | `FlowForgeEditor::Impl::requestSaveAs` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0267 | `FlowForgeEditor::Impl::saveRequests` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0268 | `FlowForgeEditor::Impl::saveStatus` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0269 | `FlowForgeEditor::Impl::retrySave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0270 | `FlowForgeEditor::Impl::abandonSave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0271 | `FlowForgeEditor::Impl::acknowledgeSave` | 函数 | P05 / P12 | SaveService 与具体 Save As 接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0272 | `FlowForgeEditor::Impl::compilation_` | 成员 | P07 / P12 | Flow 编译服务与稳定操作记录。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0273 | `FlowForgeEditor::Impl::compile_result_` | 成员 | P07 / P12 | Flow 编译服务与稳定操作记录。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0274 | `FlowForgeEditor::Impl::compile_task_` | 成员 | P07 / P12 | Flow 编译服务与稳定操作记录。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0275 | `FlowForgeEditor::Impl::requestCompile` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0276 | `FlowForgeEditor::Impl::requestPublish` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0277 | `FlowForgeEditor::Impl::compileStatus` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0278 | `FlowForgeEditor::Impl::compiled` | 函数 | P07 / P12 | 源编译/产物发布/只读状态接口。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0279 | `FlowForgeEditor::Impl::editor_` | 成员 | P10 / P12 | FlowView 与交互上下文。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0280 | `FlowForgeEditor::Impl::content_` | 成员 | P10 / P12 | FlowView 与交互上下文。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0281 | `FlowForgeEditor::Impl::createContent` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0282 | `FlowForgeEditor::Impl::finishContentEditing` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0283 | `FlowForgeEditor::Impl::applyContentIntents` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0284 | `FlowForgeEditor::Impl::contentCommand` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0285 | `FlowForgeEditor::Impl::update` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0286 | `FlowForgeEditor::Impl::event` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0287 | `FlowForgeEditor::Impl::finishEditing` | 函数 | P10 / P12 | UI 内容绘制/命令适配，编辑由 Session 执行。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0288 | `FlowForgeEditor::Impl::asset_status_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0289 | `FlowForgeEditor::Impl::read_result_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0290 | `FlowForgeEditor::Impl::reading_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0291 | `FlowForgeEditor::Impl::candidate_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0292 | `FlowForgeEditor::Impl::candidate_history_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0293 | `FlowForgeEditor::Impl::hide_requested_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0294 | `FlowForgeEditor::Impl::close_request_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0295 | `FlowForgeEditor::Impl::close_prepared_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0296 | `FlowForgeEditor::Impl::close_decision_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0297 | `FlowForgeEditor::Impl::close_connection_` | 成员 | P12 / P12 | Open/Reload/Close 用例与 PreparedData。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0298 | `FlowForgeEditor::Impl::editor_context_` | 成员 | P12 / P12 | 窄依赖注入；操作自身完成邮箱。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0299 | `FlowForgeEditor::Impl::completion_work_` | 成员 | P12 / P12 | 窄依赖注入；操作自身完成邮箱。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0300 | `FlowForgeEditor::Impl::changeAsset` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0301 | `FlowForgeEditor::Impl::reviewAsset` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0302 | `FlowForgeEditor::Impl::startAssetChange` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0303 | `FlowForgeEditor::Impl::applyAssetChange` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0304 | `FlowForgeEditor::Impl::assetFailure` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0305 | `FlowForgeEditor::Impl::adoptCompletions` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0306 | `FlowForgeEditor::Impl::applyChanges` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0307 | `FlowForgeEditor::Impl::adoptAssetResults` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0308 | `FlowForgeEditor::Impl::project` | 函数 | P12 / P12 | 打开/重载用例；诊断按域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0336 | `FlowForgeEditor::Impl::environment_` | 成员 | P04 / P12 | FlowSession；Source environment 长于节点与历史。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0337 | `FlowForgeEditor::Impl::read_nodes_` | 成员 | P04 / P12 | FlowSession；Source environment 长于节点与历史。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0338 | `FlowForgeEditor::Impl::read_pins_` | 成员 | P04 / P12 | FlowSession；Source environment 长于节点与历史。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0339 | `FlowForgeEditor::Impl::variableReferenced` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0340 | `FlowForgeEditor::Impl::indexContent` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0341 | `FlowForgeEditor::Impl::capture` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0342 | `FlowForgeEditor::Impl::metadata` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0343 | `FlowForgeEditor::Impl::nodes` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0344 | `FlowForgeEditor::Impl::pins` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0345 | `FlowForgeEditor::Impl::links` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0346 | `FlowForgeEditor::Impl::nodeName` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0347 | `FlowForgeEditor::Impl::nodeOperation` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0348 | `FlowForgeEditor::Impl::pinName` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0349 | `FlowForgeEditor::Impl::pinType` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0350 | `FlowForgeEditor::Impl::nodeLayout` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0351 | `FlowForgeEditor::Impl::pinLiteral` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0352 | `FlowForgeEditor::Impl::setPinLiteral` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0353 | `FlowForgeEditor::Impl::rename` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0354 | `FlowForgeEditor::Impl::insertNode` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0355 | `FlowForgeEditor::Impl::captureNode` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0356 | `FlowForgeEditor::Impl::insertFunctionUse` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0357 | `FlowForgeEditor::Impl::setFunctionSignature` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0358 | `FlowForgeEditor::Impl::exports` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0359 | `FlowForgeEditor::Impl::setExports` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0360 | `FlowForgeEditor::Impl::removeNodes` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0361 | `FlowForgeEditor::Impl::connect` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0362 | `FlowForgeEditor::Impl::disconnect` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0363 | `FlowForgeEditor::Impl::moveNodes` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0364 | `FlowForgeEditor::Impl::variables` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0365 | `FlowForgeEditor::Impl::addVariable` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0366 | `FlowForgeEditor::Impl::setVariable` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0367 | `FlowForgeEditor::Impl::removeVariable` | 函数 | P04 / P12 | FlowSession 的图、变量、函数与类型化查询。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0368 | `FlowForgeEditor::Impl::acceptCompilation` | 函数 | P07 / P12 | FlowCompilationService：带 stamp 的编译与链接重试。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0369 | `FlowForgeEditor::Impl::retryLink` | 函数 | P07 / P12 | FlowCompilationService：带 stamp 的编译与链接重试。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0370 | `FlowForgeEditor::Impl::completion_pending_` | 成员 | P12 / P12 | 具体完成采用，不再由窗口维护完成标志。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### Editor::Impl

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0371 | `Editor::Impl::root` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0372 | `Editor::Impl::config_` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0373 | `Editor::Impl::project_` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0374 | `Editor::Impl::platform` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0375 | `Editor::Impl::window` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0376 | `Editor::Impl::input` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0377 | `Editor::Impl::engine` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0378 | `Editor::Impl::messages` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0379 | `Editor::Impl::presentation` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0380 | `Editor::Impl::context` | 成员 | P12 / P12 | EditorApplication 组合根 / DesktopShell / 独立 engine owner。复用资源实现；context 大入口删除；唯一 product 与准确析构顺序保留。 |
| D0381 | `Editor::Impl::exit_requested_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0382 | `Editor::Impl::reviewing_exit_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0383 | `Editor::Impl::exit_request_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0384 | `Editor::Impl::close_decisions_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0385 | `Editor::Impl::close_targets_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0386 | `Editor::Impl::close_requests_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0387 | `Editor::Impl::close_intent_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0388 | `Editor::Impl::close_purpose_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0389 | `Editor::Impl::close_decisions_pending_` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0390 | `Editor::Impl::exit_intent` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0391 | `Editor::Impl::native_close` | 成员 | P12 / P12 | ShutdownOperation / CloseSessionsOperation / CloseViewOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0392 | `Editor::Impl::outcome_` | 成员 | P12 / P12 | 启动结果与 shutdown report 分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0393 | `Editor::Impl::asset_requests_` | 成员 | P12 / P12 | OpenAssetOperation 队列 / SaveAllOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0394 | `Editor::Impl::save_all_targets_` | 成员 | P12 / P12 | OpenAssetOperation 队列 / SaveAllOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0395 | `Editor::Impl::save_all_started_` | 成员 | P12 / P12 | OpenAssetOperation 队列 / SaveAllOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0396 | `Editor::Impl::menu_target_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0397 | `Editor::Impl::active_command_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0398 | `Editor::Impl::root_command_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0399 | `Editor::Impl::menu_requests_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0400 | `Editor::Impl::menu_windows_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0401 | `Editor::Impl::menu_registrations_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0402 | `Editor::Impl::menu_commands_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0403 | `Editor::Impl::menu_error_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0404 | `Editor::Impl::menu_dirty_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0405 | `Editor::Impl::menu_removed_` | 成员 | P11 / P12 | CommandRouter / MenuPresenter / 固定命令目标。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0406 | `Editor::Impl::workspace_` | 成员 | P09 / P12 | WorkspaceStore / LayoutCatalog / RecoveryManifest / 偏好。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0407 | `Editor::Impl::unrestored_panes_` | 成员 | P09 / P12 | WorkspaceStore / LayoutCatalog / RecoveryManifest / 偏好。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0408 | `Editor::Impl::workspace_revision_` | 成员 | P09 / P12 | WorkspaceStore / LayoutCatalog / RecoveryManifest / 偏好。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0409 | `Editor::Impl::workspace_result_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0410 | `Editor::Impl::workspace_intent_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0411 | `Editor::Impl::workspace_action_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0412 | `Editor::Impl::workspace_message_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0413 | `Editor::Impl::workspace_pending_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0414 | `Editor::Impl::workspace_startup_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0415 | `Editor::Impl::workspace_task_` | 成员 | P12 / P12 | ApplyLayoutOperation / RestoreSessionOperation / 独立请求结果。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0416 | `Editor::Impl::initializeMenu` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0417 | `Editor::Impl::rebuildMenu` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0418 | `Editor::Impl::receiveMenu` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0419 | `Editor::Impl::applyMenuRequests` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0420 | `Editor::Impl::validMenuTarget` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0421 | `Editor::Impl::dispatchCommand` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0422 | `Editor::Impl::applicationCommand` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0423 | `Editor::Impl::reportMenuFailure` | 函数 | P11 / P12 | MenuPresenter 与 CommandRouter；create 传播失败。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0424 | `Editor::Impl::updateSaveAll` | 函数 | P12 / P12 | SaveAllOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0425 | `Editor::Impl::startWorkspace` | 函数 | P12 / P12 | 工作区操作；捕获与应用分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0426 | `Editor::Impl::workspaceRequest` | 函数 | P12 / P12 | 工作区操作；捕获与应用分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0427 | `Editor::Impl::applyWorkspaceResult` | 函数 | P12 / P12 | 工作区操作；捕获与应用分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0428 | `Editor::Impl::restoreWorkspace` | 函数 | P12 / P12 | 工作区操作；捕获与应用分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0429 | `Editor::Impl::captureWorkspace` | 函数 | P12 / P12 | 工作区操作；捕获与应用分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0430 | `Editor::Impl::defaultWorkspace` | 函数 | P12 / P12 | 工作区操作；捕获与应用分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0431 | `Editor::Impl::beginExitReview` | 函数 | P12 / P12 | Shutdown/Close 用例；整批 permit 后销毁。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0432 | `Editor::Impl::applyCloseDecisions` | 函数 | P12 / P12 | Shutdown/Close 用例；整批 permit 后销毁。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0433 | `Editor::Impl::commitExit` | 函数 | P12 / P12 | Shutdown/Close 用例；整批 permit 后销毁。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0434 | `Editor::Impl::cancelExit` | 函数 | P12 / P12 | Shutdown/Close 用例；整批 permit 后销毁。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0435 | `Editor::Impl::requestExit` | 函数 | P12 / P12 | Shutdown/Close 用例；整批 permit 后销毁。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0436 | `Editor::Impl::cancelNativeClose` | 函数 | P12 / P12 | Shutdown/Close 用例；整批 permit 后销毁。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0437 | `Editor::Impl::handleRequests` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0438 | `Editor::Impl::fail` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0439 | `Editor::Impl::event` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0440 | `Editor::Impl::exec` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0441 | `Editor::Impl::waitForWork` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0442 | `Editor::Impl::clearPlatformInput` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0443 | `Editor::Impl::collectInput` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0444 | `Editor::Impl::startDesktop` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0445 | `Editor::Impl::create` | 函数 | P12 / P12 | EditorApplication 固定驱动阶段 / DesktopShell。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### EditorContext

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/EditorContext.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0446 | `EditorContext::execution` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0447 | `EditorContext::engine` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0448 | `EditorContext::project` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0449 | `EditorContext::assetImporter` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0450 | `EditorContext::renderRuntime` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0451 | `EditorContext::renderResources` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0452 | `EditorContext::plugins` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0453 | `EditorContext::sceneRegistrations` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0454 | `EditorContext::componentEditors` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0455 | `EditorContext::configurationEditors` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0456 | `EditorContext::panes` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0457 | `EditorContext::installation` | 函数 | P12 / P12 | 窄依赖与组合根；不提供替代万能 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0458 | `EditorContext::setAssetEditors` | 函数 | P11 / P12 | ExtensionPublisher / SessionFactory / OpenAssetOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0459 | `EditorContext::assetEditors` | 函数 | P11 / P12 | ExtensionPublisher / SessionFactory / OpenAssetOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0460 | `EditorContext::openAsset` | 函数 | P11 / P12 | ExtensionPublisher / SessionFactory / OpenAssetOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0461 | `EditorContext::setCommands` | 函数 | P11 / P12 | ExtensionPublisher / SessionFactory / OpenAssetOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0462 | `EditorContext::commands` | 函数 | P11 / P12 | ExtensionPublisher / SessionFactory / OpenAssetOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0463 | `EditorContext::commandRevision` | 函数 | P11 / P12 | ExtensionPublisher / SessionFactory / OpenAssetOperation。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0464 | `EditorContext::taskChanged` | 成员/函数 | P10 / P12 | TaskQueryPort / TaskView。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0465 | `EditorContext::tasksReset` | 成员/函数 | P10 / P12 | TaskQueryPort / TaskView。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0466 | `EditorContext::taskRevision` | 成员/函数 | P10 / P12 | TaskQueryPort / TaskView。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0467 | `EditorContext::panes_` | 成员 | P12 / P12 | ViewHost 独立 owner；旧 Context 析构。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0468 | `EditorContext::impl_` | 成员 | P12 / P12 | ViewHost 独立 owner；旧 Context 析构。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### PaneManager

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/PaneManager.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0469 | `PaneManager::create` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0470 | `PaneManager::adopt` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0471 | `PaneManager::find` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0472 | `PaneManager::findFirst` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0473 | `PaneManager::makeId` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0474 | `PaneManager::erase` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0475 | `PaneManager::setFrozen` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0476 | `PaneManager::frozen` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0477 | `PaneManager::show` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0478 | `PaneManager::root` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0479 | `PaneManager::context` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0480 | `PaneManager::panes` | 函数 | P10 / P12 | ViewHost：采用/查找/显示与身份分开。不向工具公开立即 erase/freeze；关闭变成用例；create 工厂不自行 adopt/show。 |
| D0481 | `PaneManager::setRegistrations` | 函数 | P11 / P12 | 不可变 ExtensionSnapshot / ViewFactoryRegistration。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0482 | `PaneManager::registrations` | 函数 | P11 / P12 | 不可变 ExtensionSnapshot / ViewFactoryRegistration。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0483 | `PaneManager::revision` | 函数 | P11 / P12 | 不可变 ExtensionSnapshot / ViewFactoryRegistration。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0484 | `PaneManager::root_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0485 | `PaneManager::context_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0486 | `PaneManager::revision_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0487 | `PaneManager::frozen_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0488 | `PaneManager::retained_code_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0489 | `PaneManager::registrations_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0490 | `PaneManager::panes_` | 成员 | P12 / P12 | ViewHost 和外置代码保活，无宽 Context。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### EditHistory

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditHistory.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0491 | `EditHistory::beginSave` | 函数 | P01 / P01 | SessionState::PersistenceCheckpoint；旧产品限期桥。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0492 | `EditHistory::finishSave` | 函数 | P01 / P01 | SessionState::PersistenceCheckpoint；旧产品限期桥。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### HistorySnapshot

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0493 | `HistorySnapshot::saved` | 成员 | P01 / P01 | PersistenceCheckpoint/SaveService。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0494 | `HistorySnapshot::save_pending` | 成员 | P01 / P01 | PersistenceCheckpoint/SaveService。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0495 | `HistorySnapshot::clean` | 成员 | P01 / P01 | PersistenceCheckpoint/SaveService。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### HistoryCreateInfo

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0496 | `HistoryCreateInfo::initially_saved` | 成员 | P01 / P01 | SessionState 初始化已绑定/未命名来源。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### EHistoryEvent

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0497 | `EHistoryEvent::SAVE_STARTED` | 枚举值 | P01 / P01 | SaveService 状态通知，不是历史事件。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### EEditError

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0498 | `EEditError::SAVE_IN_PROGRESS` | 枚举值 | P01 / P01 | SaveFailure 域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0499 | `EEditError::STALE_SAVE` | 枚举值 | P01 / P01 | SaveFailure 域。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

### SceneRuntime

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/engine/scene/composition/include/lux/engine/scene/SceneRuntime.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0500 | `SceneRuntime::valid` | 函数 | P06 / P06 | SceneRuntime::resumeSimulation；P06 指定契约。不要全局替换无关 ID.valid、系统 tick 或其他资源 destroy；私有底层即时释放算法可保留。 |
| D0501 | `SceneRuntime::invalid` | 函数 | P06 / P06 | SceneRuntime::pauseSimulation；P06 指定契约。不要全局替换无关 ID.valid、系统 tick 或其他资源 destroy；私有底层即时释放算法可保留。 |
| D0502 | `SceneRuntime::tick` | 函数 | P06 / P06 | SceneRuntime::driveFrame；P06 指定契约。不要全局替换无关 ID.valid、系统 tick 或其他资源 destroy；私有底层即时释放算法可保留。 |
| D0503 | `SceneRuntime::getSceneRegistry` | 函数 | P06 / P06 | SceneRuntime::borrowInstance；P06 指定契约。不要全局替换无关 ID.valid、系统 tick 或其他资源 destroy；私有底层即时释放算法可保留。 |
| D0504 | `SceneRuntime::getClock` | 函数 | P06 / P06 | SceneRuntime::borrowClock；P06 指定契约。不要全局替换无关 ID.valid、系统 tick 或其他资源 destroy；私有底层即时释放算法可保留。 |
| D0505 | `SceneRuntime::destroy` | 函数 | P06 / P06 | SceneRuntime::retireInstance；P06 指定契约。不要全局替换无关 ID.valid、系统 tick 或其他资源 destroy；私有底层即时释放算法可保留。 |

### (全类型)

源码：[固定提交文件](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/WorkspaceRequest.hpp)。

| ID | 旧限定符 | 种类 | 首次迁移 / 最晚消失 | 接替者与具体约束 |
| --- | --- | --- | --- | --- |
| D0506 | `(全类型)::WorkspaceRequest` | 类型 | P09 / P12 | 布局查询/命令/结果分开。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0507 | `(全类型)::EWorkspaceAction` | 类型 | P09 / P12 | 具体布局操作。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0508 | `(全类型)::CloseRequest` | 类型 | P12 / P12 | CloseView/CloseSessions 与 permit。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0509 | `(全类型)::AssetEditStatus` | 类型 | P12 / P12 | Open/Reload 的具体状态。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0510 | `(全类型)::PaneRegistration` | 类型 | P11 / P12 | ViewFactoryRegistration。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |
| D0511 | `(全类型)::CommandRegistration` | 类型 | P11 / P12 | CommandDescriptor + CommandEntry。新 owner 不得保留返回旧 Impl 的反向指针；同一内容不双写。 |

## 2. 具体文件迁移与删除

移动实现必须从原 CMake/安装 source list 移除，再由新 target 唯一编译。不能让同一 .cpp 同时成为新旧二进制各自维护的业务真相。纯历史算法按 P01 在原产品中也使用新实现；领域模型新旧过渡仅允许非产品验证，P12 删除旧实现。

| ID | 原文件 | 动作 / 首次阶段 / 截止 | 目标 |
| --- | --- | --- | --- |
| F001 | [editor/editing/src/EditHistory.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/src/EditHistory.cpp) | MOVE_IMPLEMENTATION / P01 / P01 | editor/history/src/EditHistory.cpp |
| F002 | [editor/editing/src/EditTypes.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/src/EditTypes.cpp) | MOVE_IMPLEMENTATION / P01 / P01 | editor/history/src/EditTypes.cpp |
| F003 | [editor/editing/include/lux/engine/editor/editing/EditHistory.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditHistory.hpp) | MOVE_IMPLEMENTATION / P01 / P01 | editor/history/include/lux/engine/editor/editing/EditHistory.hpp |
| F004 | [editor/editing/include/lux/engine/editor/editing/EditTypes.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditTypes.hpp) | MOVE_IMPLEMENTATION / P01 / P01 | editor/history/include/lux/engine/editor/editing/EditTypes.hpp |
| F005 | [editor/editing/include/lux/engine/editor/editing/EditOperation.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/editing/EditOperation.hpp) | MOVE_IMPLEMENTATION / P01 / P01 | editor/history/include/lux/engine/editor/editing/EditOperation.hpp |
| F006 | [editor/context/include/lux/engine/editor/EditorContext.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/EditorContext.hpp) | RETIRE_FILE / P10 / P12 | 窄依赖 / ViewHost / ExtensionPublisher；context target 删除 |
| F007 | [editor/context/include/lux/engine/editor/PaneManager.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/PaneManager.hpp) | RETIRE_FILE / P10 / P12 | 窄依赖 / ViewHost / ExtensionPublisher；context target 删除 |
| F008 | [editor/context/src/EditorContext.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/src/EditorContext.cpp) | RETIRE_FILE / P10 / P12 | 窄依赖 / ViewHost / ExtensionPublisher；context target 删除 |
| F009 | [editor/context/src/PaneManager.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/src/PaneManager.cpp) | RETIRE_FILE / P10 / P12 | 窄依赖 / ViewHost / ExtensionPublisher；context target 删除 |
| F010 | [editor/context/CMakeLists.txt](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/CMakeLists.txt) | RETIRE_FILE / P10 / P12 | 窄依赖 / ViewHost / ExtensionPublisher；context target 删除 |
| F011 | [editor/app/src/Editor.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/Editor.cpp) | RETIRE_FILE / P11 / P12 | editor/application / desktop / workflows 分责，不留原 Impl |
| F012 | [editor/app/src/EditorStartup.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorStartup.cpp) | RETIRE_FILE / P11 / P12 | editor/application / desktop / workflows 分责，不留原 Impl |
| F013 | [editor/app/src/EditorMenu.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorMenu.cpp) | RETIRE_FILE / P11 / P12 | editor/application / desktop / workflows 分责，不留原 Impl |
| F014 | [editor/app/src/EditorWorkspace.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorWorkspace.cpp) | RETIRE_FILE / P09 / P12 | editor/application / desktop / workflows 分责，不留原 Impl |
| F015 | [editor/app/src/EditorWorkspaceStorage.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/src/EditorWorkspaceStorage.cpp) | RETIRE_FILE / P09 / P12 | editor/application / desktop / workflows 分责，不留原 Impl |
| F016 | [editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/pinclude/lux/engine/editor/detail/EditorImpl.hpp) | RETIRE_FILE / P09 / P12 | EditorApplication 仅装配与固定驱动 |
| F017 | [editor/app/pinclude/lux/engine/editor/detail/EditorWorkspace.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/app/pinclude/lux/engine/editor/detail/EditorWorkspace.hpp) | RETIRE_FILE / P09 / P12 | layout/recovery/store 独立值 |
| F018 | [editor/context/include/lux/engine/editor/WorkspaceRequest.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/context/include/lux/engine/editor/WorkspaceRequest.hpp) | RETIRE_FILE / P09 / P12 | 新职责类型；同一路径仅一个原文件条目 |
| F019 | [editor/editing/include/lux/engine/editor/CloseRequest.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/CloseRequest.hpp) | RETIRE_FILE / P09 / P12 | 新职责类型；同一路径仅一个原文件条目 |
| F020 | [editor/editing/include/lux/engine/editor/AssetEditing.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/editing/include/lux/engine/editor/AssetEditing.hpp) | RETIRE_FILE / P09 / P12 | 新职责类型；同一路径仅一个原文件条目 |
| F021 | [editor/metadata/include/lux/engine/editor/metadata/PaneRegistration.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/metadata/include/lux/engine/editor/metadata/PaneRegistration.hpp) | RETIRE_FILE / P09 / P12 | 新职责类型；同一路径仅一个原文件条目 |
| F022 | [editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/metadata/include/lux/engine/editor/metadata/CommandRegistration.hpp) | RETIRE_FILE / P09 / P12 | 新职责类型；同一路径仅一个原文件条目 |
| F023 | [editor/tools/scene/include/lux/engine/editor/scene/SceneEditor.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/include/lux/engine/editor/scene/SceneEditor.hpp) | RETIRE_FILE / P02 / P12 | scene/model 与 scene/ui 独立 API |
| F024 | [editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/pinclude/lux/engine/editor/scene/detail/SceneEditorImpl.hpp) | RETIRE_FILE / P02 / P12 | 各成员按 D 账本迁移；禁止新 MegaImpl |
| F025 | [editor/tools/scene/src/SceneEditor.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneEditor.cpp) | RETIRE_FILE / P02 / P12 | 各域实现与 View 实现，旧类完整删除 |
| F026 | [editor/tools/scene/src/SceneAssets.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneAssets.cpp) | RETIRE_FILE / P05 / P12 | 保存适配＋打开/重载用例 |
| F027 | [editor/tools/scene/test/SceneEditorTestAccess.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/test/SceneEditorTestAccess.hpp) | RETIRE_FILE / P10 / P12 | 迁移行为测试至公开受限 API/fault ports，不进产品库 |
| F028 | [editor/tools/scene/test/SceneEditorTestAccess.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/test/SceneEditorTestAccess.cpp) | RETIRE_FILE / P10 / P12 | 迁移行为测试至公开受限 API/fault ports，不进产品库 |
| F029 | [editor/tools/material/include/lux/engine/editor/material/MaterialEditor.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/include/lux/engine/editor/material/MaterialEditor.hpp) | RETIRE_FILE / P03 / P12 | material/model 与 material/ui 独立 API |
| F030 | [editor/tools/material/pinclude/lux/engine/editor/material/MaterialEditorImpl.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/pinclude/lux/engine/editor/material/MaterialEditorImpl.hpp) | RETIRE_FILE / P03 / P12 | 各成员按 D 账本迁移；禁止新 MegaImpl |
| F031 | [editor/tools/material/src/MaterialEditor.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/src/MaterialEditor.cpp) | RETIRE_FILE / P03 / P12 | 各域实现与 View 实现，旧类完整删除 |
| F032 | [editor/tools/material/src/MaterialAssets.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/src/MaterialAssets.cpp) | RETIRE_FILE / P05 / P12 | 保存适配＋打开/重载用例 |
| F033 | [editor/tools/material/test/MaterialEditorTestAccess.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/test/MaterialEditorTestAccess.hpp) | RETIRE_FILE / P10 / P12 | 迁移行为测试至公开受限 API/fault ports，不进产品库 |
| F034 | [editor/tools/material/test/MaterialEditorTestAccess.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/test/MaterialEditorTestAccess.cpp) | RETIRE_FILE / P10 / P12 | 迁移行为测试至公开受限 API/fault ports，不进产品库 |
| F035 | [editor/tools/flowforge/include/lux/engine/editor/flowforge/FlowForgeEditor.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/include/lux/engine/editor/flowforge/FlowForgeEditor.hpp) | RETIRE_FILE / P04 / P12 | flowforge/model 与 flowforge/ui 独立 API |
| F036 | [editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowForgeEditorImpl.hpp) | RETIRE_FILE / P04 / P12 | 各成员按 D 账本迁移；禁止新 MegaImpl |
| F037 | [editor/tools/flowforge/src/FlowForgeEditor.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/src/FlowForgeEditor.cpp) | RETIRE_FILE / P04 / P12 | 各域实现与 View 实现，旧类完整删除 |
| F038 | [editor/tools/flowforge/src/FlowAssets.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/src/FlowAssets.cpp) | RETIRE_FILE / P05 / P12 | 保存适配＋打开/重载用例 |
| F039 | [editor/tools/flowforge/test/FlowForgeEditorTestAccess.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/test/FlowForgeEditorTestAccess.hpp) | RETIRE_FILE / P10 / P12 | 迁移行为测试至公开受限 API/fault ports，不进产品库 |
| F040 | [editor/tools/flowforge/test/FlowForgeEditorTestAccess.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/test/FlowForgeEditorTestAccess.cpp) | RETIRE_FILE / P10 / P12 | 迁移行为测试至公开受限 API/fault ports，不进产品库 |
| F041 | [editor/tools/scene/src/SceneSource.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneSource.cpp) | SPLIT_THEN_RETIRE_OLD_PATH / P02 / P12 | scene/model + scene/persistence |
| F042 | [editor/tools/scene/src/SceneContent.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneContent.cpp) | SPLIT_THEN_RETIRE_OLD_PATH / P02 / P12 | scene/model + scene/projection |
| F043 | [editor/tools/scene/src/SceneObjectEdits.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneObjectEdits.cpp) | SPLIT_THEN_RETIRE_OLD_PATH / P02 / P12 | scene/model 具体操作 |
| F044 | [editor/tools/scene/src/SceneOpening.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/SceneOpening.cpp) | SPLIT_THEN_RETIRE_OLD_PATH / P05 / P12 | scene/persistence + workflows |
| F045 | [editor/tools/scene/src/ScenePlayback.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/ScenePlayback.cpp) | SPLIT_THEN_RETIRE_OLD_PATH / P06 / P12 | scene/execution |
| F046 | [editor/tools/scene/src/ModelCreation.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/scene/src/ModelCreation.cpp) | SPLIT_THEN_RETIRE_OLD_PATH / P12 / P12 | scene/integration ModelCreationOperation |
| F047 | [editor/tools/material/pinclude/lux/engine/editor/material/MaterialCompilation.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/pinclude/lux/engine/editor/material/MaterialCompilation.hpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P07 / P12 | material/preview |
| F048 | [editor/tools/material/pinclude/lux/engine/editor/material/MaterialPreview.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/material/pinclude/lux/engine/editor/material/MaterialPreview.hpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P07 / P12 | material/preview |
| F049 | [editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowCompilation.hpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/pinclude/lux/engine/editor/flowforge/FlowCompilation.hpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P07 / P12 | flowforge/compilation |
| F050 | [editor/tools/flowforge/src/ui/FlowForgeElements.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/tools/flowforge/src/ui/FlowForgeElements.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | flowforge/ui；从旧 Impl friend 访问改受限角色 |
| F051 | [editor/ui/src/TaskPane.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/TaskPane.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/tasks/ui |
| F052 | [editor/ui/src/WindowInput.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/WindowInput.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/desktop |
| F053 | [editor/ui/src/WindowOutput.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/WindowOutput.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/desktop |
| F054 | [editor/ui/src/Presentation.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/Presentation.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/desktop |
| F055 | [editor/ui/src/UiRenderSyncStage.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/UiRenderSyncStage.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/desktop |
| F056 | [editor/ui/src/SceneConfigurationElement.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/SceneConfigurationElement.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/tools/scene/ui |
| F057 | [editor/ui/src/ComponentEditors.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/ComponentEditors.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/tools/scene/ui |
| F058 | [editor/ui/src/CodegenInput.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/CodegenInput.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/tools/scene/ui |
| F059 | [editor/ui/src/SceneElement.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/SceneElement.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/tools/scene/ui + projection |
| F060 | [editor/ui/src/SpatialInteraction3D.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/SpatialInteraction3D.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/tools/scene/interaction + ui |
| F061 | [editor/ui/src/asset/AssetPickerElement.cpp](https://github.com/LUX-YU/lux-engine/blob/2bb33ff1a1f11025cf404e074c0e9b259239d8a4/editor/ui/src/asset/AssetPickerElement.cpp) | MOVE_OR_SPLIT_PRIVATE_IMPLEMENTATION / P10 / P12 | editor/project/ui |

## 3. 必须保留而不是误删的内容

保留 C++20、现有 expected/独占所有权工具、实际 EditHistory 预算与提交算法、WorldObjectId、NodeId/PinId、现有 Engine 执行器、原场景编码及项目格式读取、可复用渲染/空间查询算法。框架类型重新设计不意味着重写这些已经承担正确职责的机制。

保留运行态调试编辑能力（若 P00 确认现有可用），但其历史归 RunSession，不可偷写作者态；显式 ApplyRunChanges 未存在时不强迫在重构中新增。保留 Scene 配置、对象空间/分区、材质图与 Flow 函数/变量/编译/发布、任务与项目浏览、输入捕获和 IME 等现有产品功能，完整 feature-parity 在 P00 登记。

保留未知布局载荷、旧布局只读迁移器、真实失败日志、旧格式负例和用户备份。允许它们包含旧类型名的字符串，禁止它们包含重新编译的旧业务 owner。保留部分低层私有头机制，但不能开放整个 sibling pinclude/sinclude。

## 4. 自动审计无法替代的人工核对

每个删除项必须附原引用计数/位置、修改后的调用方和测试证据。扫描旧名字零命中不能证明没有将旧类改名为 NewContext；应检查每个新 owner 的成员表与声明依赖。向多个 Manager 注入同一可写 SharedEditorState、返回完整 Impl&、用公共 friend 绕过窄接口，都视为未完成。

新 .cpp 从生产 target 撤下却留在磁盘、改后缀、`#if 0`、backup/compat/deprecated 子目录都不能视作删除。实际需要保留的旧数据文件逐项进独立名单，不用目录名一刀切。
