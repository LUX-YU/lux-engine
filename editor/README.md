# Editor：业务入口与编辑器边界

本目录保存长期有效的模块设计说明。按日期或阶段命名的实施报告、交审记录、日志、性能样本、截图归档和临时操作清单不放入产品源码树；既有历史可通过 Git 查询。

本文记录设计责任，不作为任何阶段“已经验收通过”的声明。

Editor 是仓库顶层产品，与 modules、engine 平级。依赖方向是 editor → engine → modules，
Editor 也可直接使用 modules；engine/modules 不链接 Editor。游戏消费 engine/modules，不创建 EditorContext。
具体 target 和安装头保留 SDK 的 lux::engine / lux/engine 前缀，它们不表示源码仍在 engine 目录。

## 顶层结构

```text
Editor
  ├── EngineContext：应用执行、资产 VFS、RenderRuntime（游戏可选）
  ├── EditorContext：借用 EngineContext；拥有项目逻辑、插件/元信息、RenderResources
  ├── 窗口、布局恢复、主循环和工具退出协调
  ├── 每项目一个具体编辑器 Pane（按需创建，可为空）
  │   ├── SceneEditor
  │   ├── MaterialEditor
  │   ├── FlowForgeEditor
  │   └── 可扩展的其他编辑器
  ├── Presentation → UI render-scene → UiRenderFeature
  └── 原生窗口与输入批次
```

Editor 是业务入口，不必 is-a Window。当前产品由 app 直接装配原生窗口和 UI Presentation；具体业务与窗口依然分离，未提供无窗口 Editor 产品。

具体业务由 XxxEditor 拥有：内容、历史、请求及生命周期。EditorContext 的 PaneManager 通过 PaneRegistration 工厂激活内置或插件窗口，但不读取、解码或修改其模型。

`create(Root&, PaneId, EditorContext&)` 创建空工具；`openAsset(AssetId)` 在工具内读取和切换。
根不再维护 Document 槽表或 Opening；产品以冷装配函数登记窗口和资产路由。Scene 的 ResourcePane 激活材质资产，经父链 AssetOpenRequest
到达产品根，由 Context 的资产路由找到已打开/正在加载该资产的窗口；否则创建新的 MaterialEditor。
显式在同一工具中切换内容仍先审阅未保存修改，取消或读取失败保留原模型。
Scene、Material、FlowForge 的资源与行为已归入各自 Impl；公开类只转发业务方法，私有方法不进入安装头。
历史操作实现留在各自 cpp，UI 子对象先销毁，它们借用的模型、历史和预览资源随后释放。

## 当前模块归属

| 目录 | 职责 |
| --- | --- |
| `app` | Editor 产品创建、输入/UI 接线、窗口、主循环与退出协调 |
| `context` | 每个项目的共享设施 owner、固定装配、析构收尾；不拥有 UI/场景/历史 |
| `project` | ProjectManifest、纯 codec、ProjectBuildConfig 与 ProjectBuilder |
| `storage` | ProjectStorage、目录、打开、发布、恢复与 createProject |
| `assets` | AssetImporter、源读取/保存与编译发布工作流 |
| `launcher` | 独立启动产品、共享 ProjectCreationPane 与新 Editor 进程启动 |
| `editing` | 内容历史、保存票据、业务错误和通用 Undo／Redo 协议 |
| `tools/scene` | SceneEditor 业务、Inspector/Outliner/ResourcePane 和场景内容 |
| `tools/material`、`tools/flowforge` | 各自模型、历史、异步请求与局部 UI |
| `ui` | 共用 ViewportElement、空间交互、组件 Element 生成器、Presentation；设置/项目窗口归 tools/settings、tools/project |
| `metadata` | 编辑器元信息、组件/配置 UI 工厂登记、Editor 插件接纳 |
| `plugins` | 内置运行模块配套的编辑器元信息和配置 UI 实现；不装入游戏 |
| `modules/function/render/runtime` | 共享 RenderRuntime、View、图像引用与后端退休（位于 modules 层） |

源码按 `include/pinclude/sinclude/src` 组织；业务细分放在这些目录内部的命名空间路径，不在子模块根散落另一套 project/run/asset 等平行结构。

## 局部状态与对象关系

Pane 保存窗口状态；具体 Editor Pane 拥有业务内容与历史，其 Element 负责局部交互。RenderResources/Renderer 保存渲染生命周期。

固定成员优先直接持有；接口扩展、独立生命周期或地址稳定性确实需要间接所有权时才使用指针。PImpl、shared_ptr 和 noexcept 本身都不是责任分离或正确性证明。

业务变化使用 LuxObject 的既有信号与生命周期机制。图像引用、帧运输、运行观察和每次鼠标状态不因此改成信号广播，也不增加第二套 MessageQueue／EventBus。

通知回调可以请求关闭和标记失效，但物理删除发生在正常 owner 安全点。隐藏工具、清空或切换内容、关闭工具是三件不同的事。工具关闭按钮先结束交互再隐藏；退出项目才关闭所有资源。

## Scene 编辑的核心边界

- WorldObjectId 属于持久内容；运行时访问与空间查询使用 Entity。
- 编辑实例与 Run 隔离，SceneEditor 内容中的 ViewportElement 切换显示。
- CameraMan 是编辑器临时实体，使用通用 Camera 组件，不导出到游戏。
- 鼠标选择、拖放放置、工作平面和内容 Undo 属于编辑器。
- 射线检测是运行时能力，供 Editor 和脚本共用。
- 高亮是 RenderFeature，选择只是它的输入来源之一。

具体规则集中在 [Scene Editor 说明](tools/scene/README.md)，避免在顶层重复维护另一套协议。

## typed Inspector

组件元信息决定字段 UI，构建时生成持久 Element、Controls 和 Layout，每个组件对应工厂实现。
生成代码只编入 Editor target，不进入领域或游戏 DLL。InspectorPane 位于 SceneEditor 的私有 UI，
其它消费者可以把工厂生成的组件 Element 放入自己的内容树。

普通字段直接编辑 Registry 中的实际值，首次写入前捕获 before，结束时捕获 after，必要时登记一条历史。变化立即走组件脏通知；不维护持续同步的通用 Draft 和 preview.after 副本。

格式转换、未完成文本等控件可以有局部表示数据，但不发展成另一份权威模型。

## 推进与异步

Main 拥有 Scene 和编辑业务。Process 承担明确的有限 IO、解码和编译；结果在 owner 的正常推进点采用。Simulation 使用现有 TaskGraph。

必要 Scene 更新获得公平的提交机会，再推进前端渲染，避免 Frame 持续抢占容量导致“数值变化、画面仍是旧值”。背压保留原包，不重绘并重复执行业务。

关闭未完成保留 owner；首个原始失败与实际进度一起保留，不能因为最后资源清理成功而把失败改成成功。

## 工程约束

- 不为普通分配增加全工程 OOM 恢复机制；允许正常内存分配。
- 业务失败、失效身份、线程契约和资源关闭仍需准确表达。
- `if/for/while` 等控制体使用显式大括号，不将循环和循环体挤在一行；不同语义之间留空行。
- 不因增加一个效果就给顶层 Editor／RenderSystem 增加专用功能。
- 源码树保留模块文档、使用说明和测试；过程报告与产物不进入安装 SDK。

## 唯一推进路径

Editor 按“平台输入和完成采用 → 一次可接纳的 UI 构建 → 结束字段借用 → SceneDriver → RenderRuntime”推进。UI 背压保留同一帧，不重新调用 Pane；仍推进输入、回复和退出。作者与 Run 均使用 SceneInstance 内的一份推进记录。

SceneDriver 和场景维护不使用人工次数预算，遍历本轮固定待办并遵守真实异步就绪、容量和背压。
Process Main 完成及 RenderRuntime 传输沿各自既有容量/额度；隐藏窗口及无新 UI 帧都不停止维护。
帧固定捕获时的资源版本，Program 使用既有 FIFO 和采样依赖，不用重复 UI 构建补偿推进顺序。

## 项目和工具生命周期

共享插件目录、Runtime 装载和依赖 owner 在 `engine/project/plugins`；EditorPlugin 扩展解释留 metadata。
项目 Manifest v2 明确保存启用项，SettingPane 的插件内容只编辑下次打开项目的选择，不热替换当前代码。
EditorContext、ComponentEditorRegistry、共享导入、窗口/输入接线和根行为型 Impl 已落盘。
三个固定工具借用同一个 Context；新建内容在内存中编辑，首次保存通过 VFS 同次发布源与项目目录。
已有资产另存为分配新 AssetId，发布成功后采用新的 HistoryId，原资产保留。
退出先收齐各工具的 Save/Discard/Cancel 决定，再发起不可逆的资源关闭；取消不清空其它工具。

Context 的 RenderResources 统一共享渲染资源，固定 RenderAssetInput 保留启动版本。
Run 使用捕获时的来源，新项目版本不覆盖它；ProjectStorage 本身不依赖 Renderer。

当前实现包含 Camera 组件、Editor-only CameraMan Entity、Entity 空间查询、空间视口扩展及 Feature 自有高亮集合。平台／桌面组合的实际验收结果属于树外证据，不能由本设计说明推导。

相关说明：[Scene](../engine/scene/README.md)、[Process](../engine/process/README.md)、[渲染运行入口](../../modules/function/render/runtime/README.md)。
