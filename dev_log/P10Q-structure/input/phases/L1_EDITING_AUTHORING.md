# L1：收拢 editing 与 authoring，保持真正的纯能力闭包

目标：E0/E1 的文件、target、公开定义和依赖一致；没有因路径重组复制 History、Source、codec 或项目目录。先读 L0 计划和所有实际 target。

## 1. 推荐提交单元

L1a：E0 纯身份/历史/会话物理归并及 CMake。  
L1b：ViewInfo 与 ViewError 分开；所有即时消费者一次改齐。  
L1c：三个作者模型和布局迁移。  
L1d：项目纯 Manifest/目录/Builder 迁移、与文件活动的依赖切开。

每单元可构建再提交；不能提交一个默认产品无法配置的半移动树。暂留旧产品的配置在独立明确段保持，不把新 target 链到旧聚合包。

## 2. E0 文件归并

| 原位置 | 目标位置 | 具体处理 |
|---|---|---|
| `editor/editing/history/include/**` | `editor/editing/include/**` | 按原逻辑 include 合并；不得覆盖同名旧头 |
| `editor/editing/history/src/**` | `editor/editing/src/history/**` | 保留历史算法原体，不新建 HistoryFacade |
| `editor/editing/sessions/include/**` | `editor/editing/include/**` | Session/Stamp/Permits 唯一定义 |
| `editor/editing/sessions/src/**` | `editor/editing/src/sessions/**` | Store、State、checkpoint 原实现 |
| `editor/editing/sessions/test/**` | `editor/editing/test/sessions/**` | 修改 scope_compile 脚本路径和 include 输入 |
| `editor/contracts` 中共同值 | `editor/editing/include/...` / 相应 src | CodeLease 与纯身份保持同一类型和二进制 owner |
| history/sessions/contracts 子 CMake | `editor/editing/CMakeLists.txt` | 同一目录可定义原多个真实 target；删除已空子 CMake |

如果原 contracts 包含非纯生命周期/视图实现，不按默认归入 E0。每个符号按 D02/路径表判断。

### 必须保留的类型/函数契约

- `IEditSession` 的描述与私有关闭/内容戳查询；不向公共接口暴露 currentContent 以节省查找。
- `SessionStore` 的 reserve/prepare/publish、Store 域/slot/generation、typed access、reclaiming/dispatch 保护。
- `SessionState` 的 binding/checkpoint/gate 与原 withRead/withEdit 语义。
- `EditHistory` 的唯一算法及候选保留、NO_CHANGE、Undo/Redo、memento/code 顺序。
- `CodeLease` 现有代码 owner；不能用空 builtin lease 替换插件输入来简化迁移。

### 旧 editing 不混入新 E0

原 `AssetSave/CloseRequest/AssetEditing/EditHistoryTarget/LegacyPersistenceState` 等含旧产品责任的源码仍在原位置或按实际消费者退出。不得把它们编进 `edit_history/edit_sessions/editor_contracts`。

现有 `EditorError.hpp` 是纯错误值（含 std::any cause），可按真实跨模块需要迁入 E0 唯一轻量声明；这不是要求所有 Result 改用它。旧 header 的消费者直指同一真实定义，不能仅为一个声明继续链接 `editor_editing`。外层字符串/any 错误的代码寿命维持原契约，不在本轮顺手泛化错误框架。

## 3. ViewInfo / ViewError 拆分

1. 原 `views/ViewInfo.hpp` 中保留 `ViewId/ViewTypeId/ViewRestoreKey/ViewInfo`，移至新 E0 include 根。
2. 将 `EViewError/ViewResult/ViewCloseFailure/ViewCloseResult` 移入 `editor/workbench/desktop/include/lux/engine/editor/views/ViewError.hpp`。
3. 后者直接 include 前者；IViewHost、DetachedView、具体 Views 直接 include 后者。E0 不反 include。
4. authoring/layout 不依赖后者。若某段纯规划目前返回 EViewError，改用已有布局域错误，不通过 Workbench Result 作为全局错误。
5. 保留已有枚举值和对外行为；变化局限于契约归属。需要改调用者时在同提交改齐。
6. 布局观察继续用现有纯 ViewInfo，不新增第二套相同字段目录；Host 返回的是一次观察，不额外维护一个 Model。

此处新 E3 头可在 L1 创建，代表提前建立真实契约依赖，不等于提前做整个 L3。CMake 只给它一个已有 view_api 的明确头来源，不增加 ViewError 专用库。

## 4. 三个作者模型

整包保留内部 `include/src/edits/test` 组织，迁至：

```text
editor/authoring/scene/       ← tools/scene/model
editor/authoring/material/    ← tools/material/model
editor/authoring/flow/        ← tools/flowforge/model
```

公开 `lux::editor::flowforge` 与 include 中 flowforge 保持；物理短名 flow 不触发 schema/namespace/资产格式批量重命名。

处理步骤：
1. 沿 L0 每文件清单移动；测试/生成输入/README/CMake 同步。
2. 保留 `scene_model/material_model/flowforge_model` 名称（若 L0 实际名称不同，以实际为准，记录而非偷偷另造）。
3. PRIVATE 依赖仍是实际依赖，核对公开头中需要的 graph/schema/codec 已有 target。
4. `ScenePersistenceAccess.cpp` 等先按 D04 分类。作者本地绑定/基线操作不强行搬出。
5. 纯保存角色从本包外部迁 E2，不让 E1 include SaveService 或 Process。
6. 旧工具继续调用迁出的同一纯算法，改真实 CMake 引用；不增加 forward header。

## 5. authoring/project：模型与实际项目执行分离

正式迁入：ProjectManifest 及验证/codec、AssetCatalog 值、ProjectCatalogModel/Snapshot、ProjectBuildConfig、现有纯 ProjectBuilder。

明确不迁入：ProjectStorage 的磁盘/VFS/TaskScope、ProjectCreation 文件创建、ProjectPublication、ProjectAssetSource 的 IO 工作、真正的 toolchain 构建操作。

`editor_project` 原混合 CPP 在目标结构中如果全部属于上述纯数据/Builder，可保留这个实际 STATIC target。不要为配合旧设计错误再拆出一个空 `project_builder_execution`。

目录数据保留一个 owner：Model 内不可变 Data 包含数组和索引，Storage 组合 Model，UI 借用 Model/Snapshot。Model 的 LuxObject 变化通知是允许的基础依赖，不代表它依赖 UI。

Builder 的受保护 CPP 在独立 tracked 检出迁移；不得把用户差异作为本阶段功能。

## 6. authoring/layout

将 workspace/layout 的真实纯值/计划/验证移至 authoring/layout。需要 `layout/recovery/preferences` 主题时放同一领域内，不新建三个 ModelManager。

保持：稳定 LayoutId、ViewRestoreKey+ViewType 精确匹配、额外视图保留、预算/树验证、合法未知载荷、纯计划不打开资产。

计划的 inventory 取自调用者传入的纯观察值。不要把读取 Root 的代码移进这个层；真正执行计划由工作台和 application 组合。

## 7. CMake 与独立消费

- `editor/authoring/CMakeLists.txt` 只配置五个实际领域。
- authoring 生产 target 不得链接 Process、SceneRuntime/composition、render runtime、material compiler、flow linker、workbench 或 legacy target。
- Scene 使用既有纯 SceneAsset/Camera/schema/World materialization 不等于运行实例；按实际闭包区分，不能只按 `scene` 字符串全禁。
- 应在最小消费工程中只 find/install 对应模型，不需要配置工作台或真正 compiler 的开发包；若现有安装 find 递归引入无关包，修正依赖声明，不能仅关闭测试。
- 三种模型测试仍使用真实源、历史和 codec，不替换成 FakeSession。

## 8. L1 出口

必须通过 XL01–XL06 的对应观察：三模型 CPU 正例、禁止外层依赖负例、唯一 E0 定义、纯布局、项目目录和 Builder 行为、旧产品依然可配置。

检查更名范围：不得出现新的 SceneSource/History/MaterialGraph 实现副本、旧 alias wrapper、旧头和新头同时安装、或者 schema/数据格式变化。

交付列出所有旧文件、目标位置、删除的空 CMake、原 target 的实际 source 列表、尚存旧 editing 的精确消费者。不要以“根下还有旧 editing 头”否定已分离的 E0，也不要将它们当永久 E0。
