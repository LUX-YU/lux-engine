# L2：编辑活动归位，策略依赖契约，不依赖工作台

前置：L1 新 E0/E1 闭包通过；已有 E2 行为不重写。该批最重要的不是目录，而是 TaskMonitor 的 target 分离、保存策略独立、项目副作用与纯数据分开，以及旧依赖退出。

## 1. 活动包迁移表

| 目标包 | 正式迁入 | 本包不拥有 |
|---|---|---|
| persistence | SaveService、WriteCoordinator、SaveExecution、EncodeJob、FileArtifactStore/FilePublication | 三种作者源、Pane、运行实例、第二 Executor |
| scene | SceneSaveSource/源编码角色、RunStore/RunController、SceneProjection/Hub/ResourceStatus | viewport 相机输出、第二 Runtime 驱动 |
| material | 保存角色、MaterialCompilationService/Operation、PreviewStore、发布函数 | MaterialSource 权威、MaterialView |
| flow | 保存角色、FlowCompilationService/Operation、固定对象/链接尝试、发布函数 | 可写 FlowGraph、FlowView |
| project | ProjectStorage、实际 ProjectCreation/Publication/导入/VFS接线 | 第二 ProjectCatalog 数组/版本 |
| workspace | WorkspaceStore/LegacyWorkspaceImporter/文件迁移操作 | DockLayout 权威副本、ViewHost |
| tasks | TaskMonitor | 任务调度/执行、TaskView |
| commands | 不创建空 target | P11 才建设真正命令系统 |

`activities` 目录不要求类都改名 Activity；已有正确类型名保持。

## 2. 先处理 TaskMonitor，证明无 UI 消费

### 文件与 target

- 从 `editor/tasks/ui/include/.../TaskMonitor.hpp` 移到 `editor/activities/tasks/include/.../TaskMonitor.hpp`。
- 从 `editor/tasks/ui/src/TaskMonitor.cpp` 移到同包 src。
- 新增一个真实 CPU STATIC target，建议名 `editor_tasks`；仅链接实际 LuxObject、Process 的正式 API 和必要轻量依赖。
- `tasks_ui` 删除 Monitor.cpp，转而 PUBLIC/PRIVATE 使用新 target，按实际 header 是否暴露 Monitor 类型决定。
- 旧 `tasks-ui` 包若仍装 TaskView 是真实包，可保留；新 `editor-tasks` 安装组件暴露 Monitor，不含 view_api/ImGui。
- 更新旧 Context/TaskPane 和新 TaskView 的构造，仍借用同一个应用级 Monitor，不能各自占用 Runtime observer。

### 不允许改变

Runtime 的 taskInfos/TaskInfo 是唯一任务事实，Monitor 的 snapshot 是有版本的只读投影；不存可写任务状态机。observer 回调只标记待通知，真正通知在原 dispatch 退出后。

### 回归

两个真实 Monitor 消费者共享一次观察事实；多个 TaskView 关闭不取消外部 owner 的任务；Runtime stop/finish 仍可结清；CPU consumer 不 require Pane/ImGui/ViewHost。验证链接命令和 include 依赖，而非仅不创建 UI。

## 3. persistence：同主题内保留必要的核心与执行边界

建议保留原 `editor_persistence` 与 `editor_persistence_execution` 两个 STATIC target。

纯 core 源：SaveService/WriteCoordinator/EncodedArtifact/发布协议；不包含具体 Session、FileArtifactStore 或 ExecutionRuntime。

执行与后端：SaveExecution 使用原 TaskScope；FileArtifactStore/FilePublication 放同一活动主题的私有实现组，可以保留原真实后端 target，不为一个 CPP 新增空接口。

### 具体动作

1. 将 `editor/persistence` 搬到 `activities/persistence`；文件后端从原 storage 迁入同一主题，不复制 FilePublication 算法。
2. 保留逻辑 include 中 persistence/storage 的既有准确名字。仅物理移动时无需再发明 `/activities/` 前缀。
3. `ISaveSource`、IEncodeJob、IPreparedRebind 保持必要动态契约；本轮不再套 Provider/Adapter/Port。
4. 三类保存实现各进对应活动包，通用核心只看保存角色和冻结输入，不链接三个模型。
5. `publishEncodedArtifact` 保持一份 reserve/provideEncoded/回滚算法；各领域自由函数只负责准确的产物检查和调用。
6. 不把 source-save 的 origin 继承用于 derived artifact；不让发布产物更新作者 checkpoint。
7. 完成/取消/Unknown 记录和确认协议保持，不能用 TaskId 替掉 WriteTicket/SaveId。

### 必须回归

自撤销 describe、递归 accept/ack、回调内收集别的编码完成、不同 W1 确认时机的版本衔接、Unknown 阻塞 lane、真实文件冲突/删除、关闭后迟到结果、Save As 高水位、Export Copy 不清基线。通过旧 P05/R1/R2 真实测试复用，不新增模拟保存替代。

## 4. scene 运行和投影活动

- RunStore/Controller/RunInspectAccess/StartRunOperation 归 activities/scene；inspect API 的窄 render_client 依赖按已有资格保留，不扩成 render_runtime。
- SceneProjection/Hub 归同包领域活动；视口本地 camera/output/highlight 归 workbench/viewport。
- 保持一份 SceneRuntime、一轮 driveFrame。活动只提交 prepare/adopt/stop，不内部驱动另一帧。
- 实际实例回收和单步结果仍由 Runtime/lifetime 权威管理；RunStore 不复制 32 项结果表。
- SceneProjection 的测试若链接 viewport，迁到后配置集成测试，不能使 production scene_projection 反向链接 workbench。
- 原在 scene/projection CMake 中定义的后端绑定测试按测试能力归组，保留真实 backend 检查，不能改为只检查 key。

## 5. material/flow 活动

将源保存与编译/预览功能放同一领域主题，但保留实际 CPU/toolchain/GPU closure 的 target。**同一目录不等于一个 target 或一个总 Service。**

保留：编译 operation 不可复制/不可按值移动；服务拥有有界 operation；内部 completion state 可以共享；固定源、环境、设置、target key 不变；关闭视图不取消应用仍拥有的任务。

Flow retryLink 继续使用原 object，不能为了统一模板重新捕获当前图。不同 linker/object format 的平台能力判断保留，不能只换可执行文件名。

MaterialPreviewStore 与编译服务继续分担实际不同事实，不合并为 CompilePreviewManager。

## 6. project：移除原 UI/旧编辑链接，只保留真正项目活动

1. 将 ProjectStorage、ProjectOpenData、ProjectCreation、ProjectPublication、ProjectAssetSource 的实际副作用源码归 activities/project。
2. 依赖 authoring/project 的同一个 Model/Manifest/Builder，而不是重声明目录或 ModelChanged 类型。
3. 逐处检查对旧 `editor_editing/editor_assets/EditorContext` 的依赖。只为 EditorError 这样的纯头存在的链接，改为该头的真实 provider；不能把旧 editing 目标挂进活动允许集。
4. `editor/assets` 逐文件处理：源读取/项目导入归 project 活动，具体格式角色归对应领域，原已废弃 TAssetSave 接线不进入新正式路径。
5. 纯函数和资源绑定被旧产品继续消费时可以由旧产品依赖新目标；禁止新目标为了旧窗口包含 transition 私有头。
6. 新的 project 活动必须在没有 workbench 的消费工程中读真实项目数据、产生有界请求/结果。不能为了 CPU 消费把正常错误路径删掉。

若仍存在无法在本轮解开的完整旧用户流程，只留在原旧产品岛；必须说明哪一段是旧流程，不能将“整个新的 ProjectStorage”标成豁免以绕过层规则。

## 7. workspace：文件活动不是桌面执行器

迁入真实 Store 与旧格式只读迁移。保留稳定 ID、目标摘要、selected 恢复范围、中断重试及用户新格式修改。

不新增 `applyTo(ViewHost&)` 或让 Store 持 IViewHost。文件读取后产出既有纯 Layout/Plan/Manifest，application 将它们交工作台。P12 才闭合完整恢复产品用例，当前已有纯 effects 测试仍保留。

## 8. L2 出口

- TaskMonitor CPU 独立消费者真正链接运行，无 view_api/ImGui。
- 活动策略核心和具体角色/后端的边可从 target/编译数据库区分。
- authoring 不依赖 activities；activities 不依赖 workbench/application。
- 原 late completion/撤销/代际/错误 payload/code 保活行为保持。
- 不为了“所有活动集中”创建 ActivitiesManager 或全局 service table。
- 对仍暂留的旧项目流程逐文件给出 P12 删除责任；禁止在新目录放空壳调用旧实现。
