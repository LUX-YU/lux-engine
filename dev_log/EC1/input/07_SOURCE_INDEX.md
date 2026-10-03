# 07　源码、规范与证据范围

固定仓库：`LUX-YU/lux-engine`；提交：`54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8`。

## 1. 当前源码

“本次读取”表示在本轮调用 GitHub connector 取得内容；“固定 SHA 上下文中已读”表示同一 SHA 的实际文件内容已经出现在连续审阅上下文，本轮未重复下载。未标完整的文件只支持所列符号／片段，不据此宣称全文件审计。

| ID | 路径和正式来源 | 支持的范围 | 读取状态 |
|---|---|---|---|
| S01 | [`editor/editing/include/lux/engine/editor/editing/EditHistory.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/editing/include/lux/engine/editor/editing/EditHistory.hpp) | EditHistory public mutations | 本次读取 |
| S02 | [`editor/editing/src/history/EditHistory.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/editing/src/history/EditHistory.cpp) | Impl; execute; replay; clear; close | 本次读取完整算法段 |
| S03 | [`editor/editing/include/lux/engine/editor/editing/EditOperation.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/editing/include/lux/engine/editor/editing/EditOperation.hpp) | EditOperation; PreparedEdit; friend boundary | 本次读取 |
| S04 | [`editor/authoring/project/include/lux/engine/editor/project/ProjectManifest.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/project/include/lux/engine/editor/project/ProjectManifest.hpp) | EProjectAssetKind; ProjectAssetEntry | 固定 SHA 上下文中已读 |
| S05 | [`editor/authoring/project/src/ProjectManifest.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/project/src/ProjectManifest.cpp) | kinds; validation/codec helpers | 本次读取 1–235 行 |
| S06 | [`editor/application/src/EditorViews.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorViews.cpp) | openCaptured; makeContentView; wireContentView | 固定 SHA 上下文中已读 |
| S07 | [`editor/application/src/EditorSaving.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorSaving.cpp) | assetKind; prepareSave; askSave; cancelContentPreview | 固定 SHA 上下文中已读 |
| S08 | [`editor/application/src/EditorRecovery.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorRecovery.cpp) | recoveryType; settleRecovery; captureRecovery | 本次读取 1–220 行 |
| S09 | [`editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/pinclude/lux/engine/editor/application/EditorApplicationImpl.hpp) | ContentView; VCompiledSource; ResultIntent; WorkspaceIntent | 固定 SHA 上下文中已读 |
| S10 | [`editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/extensions/include/lux/engine/editor/extensions/EditorExtension.hpp) | V7 exports; contribute(Draft&, CodeLease) | 本次读取 |
| S11 | [`editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/extensions/include/lux/engine/editor/extensions/Contributions.hpp) | ContributionDraft; ContributionSnapshot; ContributionRegistry | 固定 SHA 上下文中已读 |
| S12 | [`editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/sessions/include/lux/engine/editor/sessions/SessionFactory.hpp) | PreparedSessionData; SessionFactoryEntry; SessionLoadJob | 固定 SHA 上下文中已读 |
| S13 | [`editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/desktop/include/lux/engine/editor/views/ViewFactory.hpp) | ViewFactoryDescriptor; ViewFactoryInput; ViewFactorySnapshot | 固定 SHA 上下文中已读 |
| S14 | [`editor/workbench/desktop/include/lux/engine/editor/views/IViewHost.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/desktop/include/lux/engine/editor/views/IViewHost.hpp) | DetachedView; PreparedViewState; ViewRequests; IViewHost | 本次读取 |
| S15 | [`editor/activities/project/include/lux/engine/editor/storage/ProjectStorage.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/project/include/lux/engine/editor/storage/ProjectStorage.hpp) | captureSource; captureAssetReads; assets; preparePublication | 本次读取 |
| S16 | [`editor/authoring/project/include/lux/engine/editor/project/ProjectCatalogModel.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/project/include/lux/engine/editor/project/ProjectCatalogModel.hpp) | ProjectCatalogSnapshot; ProjectCatalogModel | 固定 SHA 上下文中已读 |
| S17 | [`editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/tasks/include/lux/engine/editor/tasks/TaskMonitor.hpp) | Snapshot; snapshot; requestCancel; observer lifetime | 本次读取 |
| S18 | [`editor/workbench/scene/src/InspectorView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/InspectorView.cpp) | options; changeComponent; clear; rebind | 固定 SHA 上下文中已读 |
| S19 | [`editor/workbench/scene/include/lux/engine/editor/scene/InspectorView.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/include/lux/engine/editor/scene/InspectorView.hpp) | InspectorComponent colocated with concrete view | 固定 SHA 上下文中已读 |
| S20 | [`editor/workbench/scene/src/OutlinerView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/OutlinerView.cpp) | readRows; update; draw; reparent | 固定 SHA 上下文中已读 |
| S21 | [`editor/workbench/scene/src/SceneConfigurationElement.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/SceneConfigurationElement.cpp) | FeatureField; system config; encode; presetFeatures | 固定 SHA 上下文中已读 |
| S22 | [`editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/include/lux/engine/editor/scene/SceneConfigurationElement.hpp) | SceneConfigurationInputs; ConfigurationControl | 固定 SHA 上下文中已读 |
| S23 | [`editor/authoring/scene/src/SceneSource.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/authoring/scene/src/SceneSource.cpp) | objects; validate; build; capture; copyOpaque | 固定 SHA 上下文中已读 |
| S24 | [`editor/activities/scene/src/SceneProjection.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/scene/src/SceneProjection.cpp) | update; replace; instantiateAuthorProjection | 固定 SHA 上下文中已读 |
| S25 | [`editor/activities/scene/src/ModelCreationOperation.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/activities/scene/src/ModelCreationOperation.cpp) | commit dependency scan; captured catalog; codec recheck | 固定 SHA 上下文中已读 |
| S26 | [`editor/workbench/flow/src/FlowView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/flow/src/FlowView.cpp) | NodePropertiesDraft; CanvasRequest; shared delivery | 固定 SHA 上下文中已读 |
| S27 | [`editor/workbench/material/src/MaterialView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/material/src/MaterialView.cpp) | Properties; node/settings UI; delivery | 固定 SHA 上下文中已读 |
| S28 | [`engine/domain/world/description/include/lux/engine/world/WorldDescription.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/world/description/include/lux/engine/world/WorldDescription.hpp) | schemas; partitioner; partitionIndexes | 固定 SHA 上下文中已读 |
| S29 | [`engine/domain/world/README.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/world/README.md) | persistent identity; dimensions; declared design limitations | 固定 SHA 上下文中已读；设计文字不视为实现证明 |
| S30 | [`engine/domain/spatial/README.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/spatial/README.md) | partition index versus active-entity queries | 固定 SHA 上下文中已读；设计文字不视为实现证明 |
| S31 | [`engine/domain/simulation/ecs/schema/include/lux/engine/simulation/ecs/ComponentSchema.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/engine/domain/simulation/ecs/schema/include/lux/engine/simulation/ecs/ComponentSchema.hpp) | author construction; snapshot and semantic kinds | 固定 SHA 上下文中已读 |
| S32 | [`modules/function/render/client/include/lux/engine/function/render/client/core/RenderFeatureRegistration.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/function/render/client/include/lux/engine/function/render/client/core/RenderFeatureRegistration.hpp) | scene_configurable; codec | 固定 SHA 上下文中已读 |
| S33 | [`modules/function/render/client/include/lux/engine/function/render/client/core/FeatureDescriptor.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/function/render/client/include/lux/engine/function/render/client/core/FeatureDescriptor.hpp) | dependencies; conflicts; multiplicity; device profiles | 固定 SHA 上下文中已读 |
| S34 | [`modules/resource/asset/include/lux/engine/resource/asset/AssetTypeId.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/resource/asset/include/lux/engine/resource/asset/AssetTypeId.hpp) | stable type token from canonical name | 本次读取 |
| S35 | [`modules/resource/asset/include/lux/engine/resource/asset/animation/SkeletonAsset.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/resource/asset/include/lux/engine/resource/asset/animation/SkeletonAsset.hpp) | existing SkeletonAsset; lux::rdesc::Skeleton; codec | 本次读取 |
| S36 | [`docs/editor-quality.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/docs/editor-quality.md) | QR01–QR22 | 本次读取全部 QR01–QR22；末尾继承表不作为新证据 |
| S37 | [`AGENTS.md`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/AGENTS.md) | repository engineering baseline | 本次读取前 145 行；上传完整文件 LF 规范化 Git blob 与远端一致 |
| S38 | [`editor/application/src/EditorArtifacts.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorArtifacts.cpp) | artifact product composition | 本轮仅沿用已读类型与调用背景；具体全函数需 S0 再核对 |
| S39 | [`editor/application/src/EditorSceneTools.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/application/src/EditorSceneTools.cpp) | showSceneTool; runtime/author distinction | 固定 SHA 上下文中已读 |
| S40 | [`editor/workbench/scene/src/SceneView.cpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/editor/workbench/scene/src/SceneView.cpp) | model drop; projection; camera/picking | 固定 SHA 上下文中已读 |
| S41 | [`modules/resource/description/include/lux/engine/description/Skeleton.hpp`](https://github.com/LUX-YU/lux-engine/blob/54f8a56354b8cdf3caebce7dcfaa873d4da0b2e8/modules/resource/description/include/lux/engine/description/Skeleton.hpp) | Bone_t; Skeleton; parent ordering; bind transforms | 本次读取 |

## 2. 用户来源

`references/AGENTS.user.md` 是用户上传规范的原字节。哈希：`4ac02b300d99fd05ff7bcb5bced4bfc6f47b8e30caaa1a8821ace240e8f75357`。CRLF→LF 只用于比较；规范化 Git blob 等于远端 AGENTS，未改写附件。

`references/P10Q.performance.historical.md` 是已有 P10Q 专项性能记录（如果本包包含）；它绑定 `3c20910d…`，不是当前最终产品或 EC1 的新实测。原 66/100、负载与分配覆盖范围保持。

## 3. 外部语言参考（补充，不替代项目规范）

- C++ Core Guidelines C.2/C.3/C.4：类的不变量、接口与实现、普通函数／成员函数的选择。https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
- C++ 工作草案的模板约束：concept 约束表达式与语法可用性，不自动证明运行期业务语义。https://eel.is/c++draft/temp.constr

项目的命名、排版、异常、构建和阶段政策以用户 AGENTS 为准。外部参考没有被用来推翻具名谓词分组规则。

## 4. 本轮没有做的事情

没有修改仓库，没有完整 clone/AST/链接闭包审计，没有独立运行引擎、真实 SDK、DLL、GPU、IME 或性能基准，没有复算历史归档全部哈希。容器直接网络读取不可用；仓库研究通过 GitHub connector 完成。

新增问题按代码事实／设计判断／静态风险／待核对分类。本包不是已验收补丁，不以文档完整性自检代替工程验证。

## 5. 本地交付自检范围

仅校验文档文件存在、相对链接、来源编号、JSON、用户规范原字节及 ZIP 可读取性。正文中的新 API 为目标签名，不宣称已在 Lux SDK 编译。
