# Editor：Workspace、Workbench、Documents 与 Panels 重构

> 拆除 LuxEditor 上帝对象、Controller 网络、异步总服务与 Panel hook，使全部编辑功能遵循同一文档/命令/视图模型

**执行文档 08 · 重构实施版 v2.0**

| 项目 | 内容 |
| --- | --- |
| 代码基线 | `LUX-YU/lux-engine@09b2a82582550bcbe03afeef77d2591e1656a656` |
| 基线日期 | 2026-08-17 |
| 文档日期 | 2026-08-18 |
| 适用对象 | Editor、Authoring、Toolchain、UI、Preview、Flow、Material、Script 与 Content 负责人 |
| 文档地位 | 架构决议与施工合同；实施分支前移时按符号和职责重新定位，不得机械照搬行号 |

> 本版以“`modules/` 是可独立分发的公共 SDK 边界”为首要前提。任何为消除依赖环而把 Engine、Scene、Editor 或 Extension 协议下沉到 `modules/` 的做法，均视为架构回归。

> **2026-08-20 裁决更新：** Editor 不创建独立 AssetStore。临时场景也构造并注册 `SceneAsset`，复用公共 `AssetManager`、Catalog 与异步加载适配器；本文其它 Workspace/Workbench 目标不因本次 Scene Asset 施工提前推进。


## 1. 目标结构

```text
Editor
├── Workspace
│   ├── Project
│   ├── Content
│   ├── ContentIndex
│   ├── ContentWatcher
│   └── Documents
│       ├── SceneDocument
│       ├── MaterialDocument
│       ├── FlowDocument
│       └── ScriptDocument
├── Workbench
│   ├── UI
│   ├── MainMenu
│   ├── PanelCatalog
│   └── active Panels
├── optional<game::Session> play
└── PreviewScene[]
```

| 当前类型 | 目标 | 处理 |
| --- | --- | --- |
| `LuxEditor` | `lux::editor::Editor` | 产品 façade；私有 State |
| `LuxEditor::Runtime` | 删除 | 字段分配到 Editor::State、Workspace、Workbench、Preview |
| `EditorShell` | `Workbench` | UI 与 Panel composition |
| `EditorToolHost` | `Workbench::Panels` 或合入 Workbench | 删除 Host |
| `EditorTools` façade | 删除 | Workbench 提供窄 Panel API |
| `ProjectController` | `Workspace` | 工程与内容所有者 |
| `SceneController` | `Documents` / `SceneDocument` | 文档生命周期 |
| `ImportController` | `Content::import` | ImportDialog 只处理 UI |
| `AssetDeleteController` | `Content::remove` | 删除确认与业务分离 |
| `AssetRegistry` | `ContentIndex` | Authoring 文件索引 |
| `AssetFileWatcher` | `ContentWatcher` | 文件事件→Content 变更 |
| `AssetBrowser` | `ContentBrowser` | View |
| `EditorAsyncService` | 删除 | Operation 按领域归位 |
| `FlowForgeCompilerService` | `toolchain::FlowCompiler` | Panel 不直接拥有 |
| `MaterialGraphPanel` | `MaterialEditor` | Document+Commands View |
| `FlowGraphPanel` | `FlowEditor` | Document+Commands View |
| `LuaConsole` | `ScriptEditor` 或 `ScriptConsole` | 不承担 Script Runtime ownership |
| `SceneFeatureSettingPanel` | `SceneSettings` | View |
| `MaterialPreviewHost` | `MaterialPreview` | 基于 PreviewScene |
| `ThumbnailService` | `Thumbnails` | 基于 PreviewScene |
| `EditorRenderInfra` | 删除 | 精确 Renderer/View/Asset dependencies |

## 2. `LuxEditor::Runtime` 解体

### 2.1 当前问题

当前 `LuxEditor::Runtime` 同时拥有：

```text
事件、渲染、资产、异步、扩展
项目、文件监视、场景
UI、所有 Panels、所有 Controllers
Flow/Material compiler
Preview/Thumbnail
hooks、subscriptions、pending actions
```

并依赖字段声明顺序形成隐式销毁图。

### 2.2 迁移方法

先建立 `Editor::State`，但明确它只是 `.cpp` 私有存储：

```cpp
struct Editor::State
{
    // process/product facilities
    Events events;
    Executor executor;
    AssetStore assets;
    Renderer renderer;
    ExtensionLoader extensions;
    Input input;

    // editor domains
    Workspace workspace;
    Workbench workbench;
    PreviewPool previews;

    std::unique_ptr<game::Session> play;
    EditorFrames frames;
};
```

禁止：

```cpp
state.get<T>();
panel(Editor::State&);
controller(Editor&);
```

### 2.3 字段迁移规则

| 字段类型 | 目标 owner |
| --- | --- |
| Project、AssetRegistry、FileWatcher | Workspace |
| Panels、Menu、UI、ToolHost | Workbench |
| Flow/Material compiler clients | Toolchain compiler owners/Workspace |
| Thumbnail、Material Preview | PreviewPool |
| Scene Edit state | SceneDocument |
| Play Scene | `game::Session` |
| Renderer/Executor/AssetStore | Editor::State |
| subscriptions | 最接近订阅者的 owner |

## 3. Workspace

### 3.1 API

```cpp
class Workspace final
{
public:
    static expected<Workspace, OpenError>
    open(
        ProjectPath,
        WorkspaceDependencies);

    Project& project() noexcept;
    Content& content() noexcept;
    ContentIndex& index() noexcept;
    Documents& documents() noexcept;

    expected<void, SaveError> saveAll();
    CloseTask close();

private:
    Project project_;
    ContentIndex index_;
    Content content_;
    ContentWatcher watcher_;
    Documents documents_;
};
```

Workspace 不 include：

```text
Panel.hpp
ImGui
ViewportPanel
UISystem/UI
EditorMenuBar
```

### 3.2 `ProjectController`

MOVE 领域状态和操作到 Workspace：

```text
openProject
closeProject
saveProject
currentProject
cache directory
content root
module requirements
```

Project switching UI 留在 Workbench：

```text
File menu
confirmation modal
file dialog
```

Workbench 调用 `Workspace::open` 或产品层 replacement，不让 Workspace 反向调用 UI。

## 4. Content 与 ContentIndex

### 4.1 `AssetRegistry → ContentIndex`

ContentIndex 只负责 Authoring 磁盘索引：

```cpp
struct ContentEntry
{
    asset::AssetId id;
    asset::AssetTypeId type;
    std::filesystem::path sourcePath;
    VirtualPath virtualPath;
    std::string displayName;
};

class ContentIndex
{
public:
    expected<void, ScanError> rebuild();
    const ContentEntry* find(asset::AssetId) const noexcept;
    std::span<const ContentEntry> children(VirtualPathView) const;
};
```

它不拥有 Runtime Decoded Assets。

### 4.2 `Content`

集中命令：

```cpp
class Content final
{
public:
    ImportTicket import(ImportRequest);
    expected<asset::AssetId, CreateError> create(CreateRequest);
    expected<void, RemoveError> remove(asset::AssetId);
    expected<void, MoveError> move(asset::AssetId, VirtualPathView);
    expected<void, RenameError> rename(asset::AssetId, std::string_view);
};
```

迁入当前散布在：

```text
EditorShell::wireAssetServices
ImportController
AssetDeleteController
AssetBrowser create/delete hooks
```

中的文件操作与事件发布。

### 4.3 事件

命令完成后 Content 发布事实：

```text
ContentImported
ContentCreated
ContentRemoved
ContentMoved
ContentRenamed
```

事件不承载请求，也不返回结果。

## 5. Documents

### 5.1 通用合同

```cpp
class Document
{
public:
    virtual ~Document() = default;
    virtual DocumentId id() const noexcept = 0;
    virtual asset::AssetId asset() const noexcept = 0;
    virtual bool dirty() const noexcept = 0;
    virtual expected<void, SaveError> save() = 0;
};

class Documents final
{
public:
    OpenTicket open(asset::AssetId);
    Document* find(DocumentId) noexcept;
    expected<void, CloseError> close(DocumentId, CloseMode);
};
```

避免建立万能虚基类后让所有编辑器依赖大量虚函数；可用 variant/typed handles。核心要求是统一文档生命周期和 dirty/save/close 语义。

### 5.2 文档与 View 分离

每个领域遵循：

```text
Document       唯一可编辑状态
Commands       修改 Document 的用例
Compiler       消费不可变 Snapshot
Editor Panel   绘制与用户输入
Codec/Repository 读取写入
```

Panel 不拥有 AssetManager、Events、Executor、Project 路径。

## 6. Workbench

### 6.1 文件迁移

```text
EditorShell.*        → Workbench.*
EditorMenuBar.*      → Workbench/MainMenu.*
EditorToolHost.*     → Workbench/Panels.*
EditorTools.*        → 删除
EditorPanelCatalog.* → PanelCatalog.*
```

### 6.2 API

```cpp
class Workbench final
{
public:
    Workbench(UI&, Workspace&, PanelCatalog&);

    PanelHandle openPanel(PanelIdView);
    void closePanel(PanelHandle);
    Panel* activePanel() noexcept;

    void frame();

private:
    UI& ui_;
    Workspace& workspace_;
    PanelCatalog& catalog_;
    PanelSet panels_;
    MainMenu menu_;
};
```

Workbench 可以知道 Workspace 的用例 API，但不进行 Asset Codec、Scene Cook 或编译。

### 6.3 Panel Factory Context

允许在唯一装配边界使用类型擦除：

```cpp
descriptor.create = [](const PanelContext& context)
{
    auto& documents = context.require<Documents>();
    auto& commands = context.require<FlowCommands>();
    return std::make_unique<FlowEditor>(documents, commands);
};
```

限制：

- Context 只在 Factory 调用期间存在；
- Panel 构造后不得保存 Context；
- Descriptor 明确列出 required types；
- 缺失依赖返回结构化错误；
- 普通业务代码不得调用 `context.find<T>()`。

## 7. 消除 Hook 与两阶段构造

### 7.1 禁止接口

```text
setAssetServices
setPreviewHost
setCompileDispatch
setPrecompileHook
setCreateMenuHook
setDeleteAssetHandler
setActivateHandler
setSceneSettingsAccessor
setAvailableComponentsProvider
```

### 7.2 替代方式

| 当前 hook | 替代 |
| --- | --- |
| Save/Create/Delete | `Content` 或 Document Command 方法 |
| Compile | Compiler Client |
| Panel activation | Workbench 直接方法 |
| 已提交事实 | Events |
| Extension panel | Panel Descriptor |
| Preview update | MaterialCommands/Preview API |
| Scene settings access | `SceneDocument&` |

必需依赖构造注入；可选当前 Document 使用 `DocumentHandle` 或 `optional<reference_wrapper<T>>`，而不是后续 setter。

## 8. Flow 全链路

### 8.1 目标文件

```text
engine/authoring/flowforge/
    FlowDocument.hpp/.cpp
    FlowCodec.hpp/.cpp

engine/toolchain/flowforge/
    FlowCompiler.hpp/.cpp
    FlowCompileRequest.hpp

engine/editor/src/flow/
    FlowCommands.hpp/.cpp
    FlowEditor.hpp/.cpp
    FlowGraphView.hpp/.cpp
    FlowSchema.hpp/.cpp
```

### 8.2 `FlowDocument`

```cpp
class FlowDocument final
{
public:
    flowforge::FlowGraph& graph() noexcept;
    const flowforge::FlowGraph& graph() const noexcept;

    bool dirty() const noexcept;
    FlowSnapshot snapshot() const;

private:
    flowforge::FlowGraph graph_;
    DirtyState dirty_;
};
```

### 8.3 Compiler

```cpp
class FlowCompiler final
{
public:
    CompileTicket compile(FlowCompileRequest);

private:
    execution::OperationClient<CompileFlowOperation> client_;
};
```

请求拥有完整快照和输出路径；后台线程不访问 Panel、Workspace 当前状态或 AssetManager。

### 8.4 Editor

```cpp
class FlowEditor final : public ui::Panel
{
public:
    FlowEditor(
        FlowDocument&,
        FlowCommands&,
        const FlowNodeCatalog&);

private:
    FlowDocument& document_;
    FlowCommands& commands_;
    const FlowNodeCatalog& nodes_;
};
```

DELETE：

```text
FlowGraphPanel::setAssetServices
FlowGraphPanel::setPrecompileHook
NodeRegistry::global()
FlowForgeCompilerService::setPrecompileDispatch
```

`NodeRegistry::global()` 改为 Workspace/Extension 注册形成的 `FlowNodeCatalog`。

## 9. Material 全链路

### 9.1 当前问题

`MaterialGraphPanel` 同时拥有：

```text
graph SSOT
compile job/outcome
Asset save/open
Material instance chain
Preview
Texture picker
Compile dispatch
GLSL/SPIR-V
```

### 9.2 目标文件

```text
engine/authoring/material/
    MaterialDocument.hpp/.cpp
    MaterialInstanceDocument.hpp/.cpp
    MaterialCodec.hpp/.cpp

engine/toolchain/material/
    MaterialCompiler.hpp/.cpp
    MaterialCompileRequest.hpp
    CompiledMaterial.hpp

engine/editor/src/material/
    MaterialCommands.hpp/.cpp
    MaterialEditor.hpp/.cpp
    MaterialGraphView.hpp/.cpp
    MaterialSchema.hpp/.cpp
    MaterialPreview.hpp/.cpp
```

### 9.3 Snapshot 与 latest-wins

保留当前后台编译和 latest-wins 思路，但移出 Panel：

```cpp
struct MaterialCompileRequest
{
    DocumentRevision revision;
    MaterialGraphSnapshot graph;
    TextureBindings textures;
};

struct CompiledMaterial
{
    DocumentRevision revision;
    std::string glsl;
    SpirvBundle spirv;
};
```

`MaterialCommands` 保存 pending revision，过期结果丢弃。

### 9.4 Instance

Material 与 Material Instance 是两个 Document 类型；可以共享一个 Editor View，但不在一个 Panel 中堆积所有状态机。

推荐：

```text
MaterialEditor
  variant<MaterialDocument*, MaterialInstanceDocument*>
```

每个 Document 自己负责父链解析与 dirty state；Preview 只接收 EffectiveMaterial Snapshot。

## 10. Script

`LuaConsole` 若主要用于交互式调用，命名为 `ScriptConsole`；若用于编辑文件，建立 `ScriptEditor`。

Panel 不拥有 `ScriptRuntime`；通过 `ScriptCommands`：

```cpp
class ScriptCommands
{
public:
    RunTicket runSelection(ScriptDocument&, TextRange);
    expected<void, SaveError> save(ScriptDocument&);
};
```

Game/Play Session 的 Script Runtime 与 Editor Console 的 Tooling Runtime 必须是不同实例或明确不同 scope，不能共享隐式全局 Registry。

## 11. SceneDocument

### 11.1 拆 `EditorScene`

当前 `EditorScene` 包含 Edit World、交互、Play、Cook、渲染与生命周期。目标拆为：

```text
SceneDocument          Authoring state、selection、undo、save
SceneCommands          create/delete/reparent/component edit
SceneCooker            Toolchain
SceneViewport          View/interaction
game::Session          Play
```

`SceneDocument` 不拥有 Runtime Scene。

### 11.2 Enter Play

Workbench 用户动作：

```text
SceneCommands::requestPlay
→ Workspace saves/validates
→ SceneCooker
→ Editor creates game::Session
→ SceneViewport retargets to play SceneView
```

退出 Play 后 retarget 回 Edit Preview，不通过 setter 修改大量裸指针；使用明确 `ViewportSource` variant/handle。

## 12. Preview 与 Thumbnail

### 12.1 `PreviewScene`

CREATE：

```cpp
class PreviewScene final
{
public:
    static expected<PreviewScene, OpenError>
    open(
        PreviewTemplate,
        Renderer&,
        Executor&,
        AssetStore&,
        SceneFeatureCatalog&);

    Scene& scene() noexcept;
    render::OffscreenView& view() noexcept;
    CloseTask close();
};
```

### 12.2 迁移

```text
MaterialPreviewHost → MaterialPreview
ThumbnailService RuntimeHost → PreviewScene
PreviewWorldCommon → PreviewTemplate/PreviewScene builder
```

MaterialPreview 与 Thumbnails 共享创建、关闭、View 与 Scene Feature 装配，不再各自实现私有 SceneRuntime Host。

### 12.3 生命周期

统一：

```text
open
update/request
close
```

删除同一对象上的：

```text
initialize
releaseGpu
shutdown
```

## 13. 裸指针处理

### 13.1 改为引用

当前构造参数为引用且对象不允许为空：

```text
FlowGraphView::graph_
FlowGraphView::registry_
FlowSchema::graph_
FlowSchema::registry_
FlowSchema::view_
MaterialGraphView::graph_
MaterialGraphSchema::graph_
MaterialGraphSchema::view_
```

改为：

```cpp
FlowGraph& graph_;
NodeCatalog& nodes_;
```

### 13.2 保留可选指针

瞬时查询：

```cpp
Document* activeDocument() noexcept;
Panel* activePanel() noexcept;
```

可以返回指针；调用者不得跨异步任务保存。

### 13.3 动态对象

Panel/Document/Extension 使用 generation handle，不以裸指针跨帧缓存。

## 14. CMake 与文件迁移

MODIFY `engine/editor/CMakeLists.txt`：

- 删除 `EditorAsyncService.cpp`；
- 删除 Controller 源；
- 新增 Workspace/Content/Documents；
- 新增 Flow/Material/Script 领域目录；
- PreviewScene 公共实现单独源组；
- 不再通过 `src` 私有 include root 随意跨目录 include；
- 对每个内部领域建立明确 private target 或 source group。

推荐内部目标：

```text
lux_editor_workspace
lux_editor_workbench
lux_editor_documents
lux_editor_flow
lux_editor_material
lux_editor_preview
lux_editor_product
```

它们是 Editor 内部目标，不安装为 Modules SDK。

## 15. Pull Request 序列

| PR | 内容 | 退出闸门 |
| --- | --- | --- |
| EDIT-01 | `Editor` façade 与 State | 行为不变 |
| EDIT-02 | ContentIndex/Content 建立 | create/import/delete 测试 |
| EDIT-03 | Workspace 替代 ProjectController | 项目切换测试 |
| EDIT-04 | Documents 与 SceneDocument | dirty/save/close 测试 |
| EDIT-05 | Workbench 替代 Shell/ToolHost | Panel 生命周期测试 |
| EDIT-06 | 删除 EditorAsyncService，Operation 归领域 | executor close 测试 |
| FLOW-01 | FlowDocument/Commands/Compiler/Editor | 删除 hooks |
| MAT-01 | MaterialDocument/Compiler/Editor | compile/preview/save 测试 |
| SCRIPT-01 | ScriptDocument/Commands/Console | Runtime scope 测试 |
| PREVIEW-01 | PreviewScene 统一 | thumbnail/material preview |
| EDIT-FINAL | 删除旧 Controllers、Shell、Runtime、hook API | 架构扫描归零 |

## 16. 验收闸门

- [ ] `LuxEditor::Runtime` 已删除。
- [ ] Panel 不持有 `Editor&`、通用 Runtime 或通用 Service Context。
- [ ] 必需依赖无 `set*Service()`/`set*Hook()`。
- [ ] `EditorAsyncService` 已删除。
- [ ] Workspace 不 include UI。
- [ ] Workbench 不执行文件 Codec/编译。
- [ ] Document 是可编辑状态唯一来源。
- [ ] Compiler 只消费 owning Snapshot。
- [ ] `NodeRegistry::global()` 已删除。
- [ ] Material Preview 与 Thumbnail 共用 PreviewScene。
- [ ] `EditorScene` 已拆分，Play 使用 `game::Session`。
- [ ] 长生命周期异步闭包不捕获 Panel/Controller 裸指针。
