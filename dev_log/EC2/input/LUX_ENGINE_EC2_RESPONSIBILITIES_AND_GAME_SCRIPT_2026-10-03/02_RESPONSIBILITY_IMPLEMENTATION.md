# 02　职责迁移的具体设计与删除要求

## 1. RD01：ProjectPublicationPlan 与 PreparedProjectPublication

### 当前起点

文件：`editor/activities/project/include/lux/engine/editor/storage/ProjectPublication.hpp`、`src/ProjectStorage.cpp`、`src/ProjectPublicationOperation.cpp`。新 HEAD 的 public publication 定义仍混有固定计划与 private owner。[C05]

### 目标类型（新名称均为施工目标）

- `ProjectUpdate`：请求值，允许调用方在准入前编辑。
- `ProjectPublicationPlan`：准备成功后的固定 manifest、目标、版本、文件与包目录；私有数据/只读观察。不能改完 manifest 却继续使用旧 manifest_bytes。
- `PreparedProjectPublication`：move-only 的完整拥有单元，组合 plan 与唯一 owner 线程 reservation；能放弃但不能复制。可在同一分配中存放，不要求为两种语义新增两次堆分配。
- `ProjectPublicationOperation`：沿用现有执行者，拥有 prepared 对象、任务和真实结果；不新增同义 ProjectPublisher。
- `ProjectPublicationReceipt`：已发生的文件/清单事实以及采用输入；与 plan identity/precondition 对应，不因 UI 状态推导。

### 计划、占用与销毁

1. 先验证输入、预算、路径、immutable reuse 和完整候选，再取得/发布占用；不能失败后留下 publishing_。
2. Plan 给 worker 的只读拥有视图不具有解除 reservation 的权限。worker 不保存 `ProjectStorage*`。
3. Prepared 的最终释放在原 owner 有效且调用保护内进行。不同 executor 不得建立独立 busy 来控制同一项目。
4. move construction 只转移一次权限；move assignment 必须先将旧完整拥有单元移至局部，按规定顺序清理。禁止字段逐个覆盖导致旧 code/source 提前失效。
5. 放弃尚未开始的计划可以释放占用；在途发布由原 Operation 等到真实结果/Unknown reconciliation，不得把析构当作磁盘回滚。
6. 提交 manifest 后的采用失败是可诊断/可重试的已发生事实，不允许重新生成新的计划冒充第一次成功。
7. 同一 immutable 资产包可被重用与可变作者保存不同；不合并两者的版本策略。

### 精确处置

删除现有 `ProjectPublication` 的 public 可写计划字段接口与旧名称的兼容入口。迁移 ProjectStorage、原 Operation、导入、派生产物、项目创建/设置、所有 SDK 和测试调用。

先查 `publishProjectFiles()` 的生产消费者：若是保留的崩溃恢复协议则复用；若与 Operation 重复实现同一提交序列，合并实际文件步骤。不能按名字删除恢复 helper，也不能同时维护第二份业务顺序。

ProjectWriteLease 与 Prepared 的发布占用不是同一权限，前者是 OS 项目写锁，后者是本 owner 的有序业务准入。两者保留准确分工，不合并成一个 bool。

## 2. RD02：SceneConfigurationDraft 与准备函数

### 目标

```text
已有配置/新建参数
    -> 拥有型 SceneConfigurationDraft
    -> prepareSceneConfiguration(draft, 固定注册/能力描述)
    -> 原 Scene/Simulation Builders
    -> 正式配置/SceneSetConfiguration

UI 只维护草稿、结束控件编辑、展示诊断、发出领域提交
```

### 草稿必须包含

场景显示名称（仅实际可改时）、声明 schema、稳定 SystemInstanceId、系统类型/配置 schema 与版本/拥有字节、provider 绑定、construction/execution/scene/channel 关系、viewport 选择，以及修改现有配置时的原内容戳/原描述保留。

不要用 `map<string,any>` 代替这些语义；不要每种字段创建独立模块。优先复用引擎已有的描述值和边类型。未知系统/Feature 配置字节与原版本完整保留，不能因 UI 没控件就写默认值覆盖。

### 精确迁移

- 从 `SceneConfigurationElement.cpp` 迁出 `SystemElement` 中正式描述构造、默认 Feature 配方选择、provider 解析、关系验证；私有控件类保留布局和局部输入。
- `build()` 不再读取全部控件并成为唯一编译器。改为控件结束编辑后捕获 draft，再调用同一准备函数；不要留下两个 build 实现。
- `load()` 分为正规配置到 draft、draft 安装到离树控件两步。失败保持原挂载表单/source/code owner。
- `SceneCreationConfiguration` 从 UI 头迁到其真实纯值/准备提供者；拆掉 `CONTROL_FAILURE` 与模型准备错误混装，UI 可以汇总两者。
- `SceneProviderOption` 的 string_view 仅用于同调用借用；跨帧 draft/准备结果要拥有名称或保有固定注册快照。不能从临时容器借用。
- 复用 EC1 的 `AuthoringFacts/queryApplicability` 与原 Builder，不复制依赖解析和 Is2D/Is3D 表。

### 实际分区与预设

当前 UI 新建明确只提供 single partition，且 `systemOptions` 仍以 single 查询。[C08] 新纯函数应显式接受目标分区契约：

- 新建默认 single 是产品预设，不是所有来源的隐式事实。
- 对既有 indexed World 不支持的结构编辑继续准确拒绝，不把列出候选当成新增索引编辑功能。
- 2D/3D 预设只是生成一份初始 draft；不影响正交/透视、活动查询后端、分区存储的独立含义。
- 已存在的不适用/未知配置应显示并保留诊断，不能静默丢弃。

草稿/只依赖纯描述的函数归 `authoring/scene`；需要运行注册列表/Feature default codec 的准备归 `activities/scene`，但仍不得要求实际 Renderer 或 Root。工作台转输入。**这些 Editor 草稿不自动成为游戏脚本 API**；游戏直接使用引擎已有运行描述或能力。

## 3. RD03：把派生产物发布从 Application 迁回实际活动

当前已使用 `DerivedArtifact`，不要恢复 VCompiledSource 或再创建并行 AnyCompiledObject。[C04]

### 活动入口

由现有项目/持久化提供者接收：固定 DerivedArtifact、准确的源绑定/采用政策、目标项目与预算。返回稳定操作 ID 或原拥有型操作。输入拒绝不丢调用方唯一载荷；接受后 UI 可以消失，操作仍有 owner。

### 原流程应迁走的部分

`EditorArtifacts.cpp::settleArtifacts()` 中的：

- 源 key/binding 与目标验证；
- package 编码与源摘要；
- 文件目标 reserve/provide/settle/reconcile；
- 已发布包目录读取；
- manifest 准备/写入/采用/确认；
- 编码和读取阶段的回调结果记录。

保留 Application 的产品动作选择、结果观察及退出协调，不在 Impl 中维护另一个上述状态机。

### 复用策略必须先实现再迁消费者

1. 审核 `ProjectPublicationOperation` 已有 FILES/PACKAGES/MANIFEST/ADOPT 顺序。
2. 提供固定包输入及“已发布但尚未登记”结果时，扩展原执行者的准确输入/阶段，不重新发布同一文件。
3. 如果现有唯一多文件协议和 derived 提交的不同源保存政策不能直接共用，在原模块提取共同发布步骤，两个有明确领域语义的入口组合它；不可复制核心算法。
4. 源在编译后继续保存时，派生产物目录采用只能更新其 cooked 事实，不倒退 source_digest/checkpoint。
5. 继续支持“文件成功、目录失败”、Unknown、取消后晚到结果和明确确认；结果查询不会因为窗口关闭而消失。

`ArtifactPresentation` 最终只保留显示所需的操作 ID/拥有型只读摘要，不再含执行所需的两个 ticket、多组 encoding/reading 状态和私有 Task。

## 4. RD04：WorkspacePane / ResultsPane 不再使用 AppImpl

### 最终约束

通用面板能在独立安装消费者中构造，不包含 `EditorApplicationImpl.hpp`；原内置面板使用同一个公开入口。

- WorkspaceView 消费布局目录/迁移结果的只读观察，发出类型化 Workspace 请求。
- ResultsView 消费来源明确的 Save/Run/Import/Publication 观察，并发送现有类型化动作或命令。
- 跨层的打开显示/恢复/退出继续由 Application 组合，不能为“面板都在 workbench”而将 ViewHost 拉进底层内容服务。

不新增 `IApplicationAccess`、巨大 getters 接口、SharedApplicationState。已有具体 provider 足够时直接注入引用。确需跨域呈现汇总时只提供 immutable summary 和有限动作集合；它不拥有第二份业务结果、不自行确认底层记录。

### 更新和线程

- UI 回调只入意图，原 owner 在安全点执行；队列满必须明确，不重发部分已交付动作。
- draw 不读取文件、不启动编码、不调用 `waitUntil`。
- 若已有 revision，可在 update 中生成一次共享展示快照；没有收益证据不每字段加缓存。
- snapshot 失败保留上次显示并标记暂时不可用，不报告“空列表”。
- 终态结果与行 ID 有完整域/代际关联，不能只取 slot 或把 uint64 截成 int 用作业务身份。

原 `VResultIntent/VWorkspaceIntent` 保持动作绑定载荷，迁到恰当实际契约位置，旧 private 定义同批删除。

## 5. RD05：MaterialPreview 的配方、产物和采用

### 目标分工

- **MaterialCompileInputKey**（目标名）：content/settings/environment，描述编译什么。
- **CompiledMaterial**：经正确构造的固定输入、artifact 与 bytes；只读关联，不带某个窗口的唯一 target。
- **PreviewAdoptionKey**（目标名）：完整预览目标身份/代次、期望输入、配方/环境代次，属于单预览采用。
- **MaterialPreview**：现有单目标活预览 owner 的最终语义名，继续拥有 runtime lease、候选、已接受状态、相机和退休。
- **预览配方/构造函数**：现有纯 ScenePackage 或紧凑 recipe；默认球体是一个内置配方，不在资源采用循环中决定。

### 必须迁移的算法和接口

- 将 sphereImage/previewWorld/默认 Feature、灯光和相机构造移到同主题配方源文件，不放入另一个全局 PreviewManager。
- `receive(const MaterialCompileOperation&, ...)` 改为接收拥有型完成结果与目标采用关联。编译 operation 仍独占控制权，预览不取消或窥探 task。
- 错误拆成编译错误与预览/资源错误；navigate/reset 不再伪装为编译请求失败。
- 原 target 参数从纯编译结果身份退出；每个目标分别保持期望代次，旧完成无法覆盖新目标。不得简单删除 target 判断。
- 两个预览可消费同一固定编译产物；目标分别独立，关闭其中一个不能销毁另一个所需的资源。
- `CompiledMaterial/CompiledFlow` 关联由 compiler/合法工厂构造，公共只读访问。不要允许外部随意更换 bytes 却保留另一份 artifact/key。

不要求新的全局编译缓存。共享同一结果的能力与自动去重是两件事。编译 settings 中真正影响输出或成功条件的部分保持 key 语义，不为删 target 顺带改变环境/额度的验证。

## 6. RD06：Workspace 数据转换、政策与 IO

将原 `LegacyWorkspaceImporter.cpp` 内的纯 TOML/INI→布局/恢复转换提取为可对固定字节调用的函数。输入应拥有每个文件的相对路径、字节和已验证版本；输出为固定迁移计划。

存储负责采集真实输入及验证 current version；迁移推进复用原 coordinator 与 marker。每次重试都必须证明来源仍相同，但版本未变时不必重复全部解析。目录枚举不完整、权限失败不得变为空目录。

- 只选明确 selected 的旧布局产生恢复，所有合法布局保留。
- 已同源写入的新布局、用户后来修改的 label/recovery、完成 marker 不能被升级静默覆盖。
- `layoutResult()` 与 `LayoutCommitReceipt` 的 pending + catalog 混合接口退出：使用原 WriteCoordinator status，显式 `listLayouts/refresh` 做 IO。
- 不新增一个只有一行 status 转发的 WorkspaceStatusManager。
- 纯 fallback 决策可以单独函数；便利的读+选择入口名称明确、成本明确。

## 7. RD07/RD08：模型 recipe 与 Project 打开准备

R0 核对当前剩余算法后：

- 将 ModelImportRecipe 的值、decode/encode 从 AssetImporter 的 Load/Encode 内提取，放原模型导入主题；复用 ModelCooker。
- 原 AssetImporter 若确实只有模型能力，最终以 ModelImportOperation/ModelImporter 的单一准确名称提供（R0 根据其是一请求 owner 还是多记录 owner 选择一个），不同时保留两个同义公开入口。
- 字节读取、CPU cook、项目发表仍用原 Process 和发布者；不建立 ImporterRegistry，除非本轮真实第二种导入消费者无法用现有流程表达。
- ProjectOpenData 若仍含 write lease，改为拥有型 `PreparedProjectOpen` 并封装消费规则。准备值不等于可复制 POD；worker 可生成尚未采用的拥有结果，但不能销毁活 Project。
- 名称变化同步安装 SDK、生成器、测试和应用，不留下 alias。

## 8. 本轮明确不拆

EditHistoryData/EditExecutor（EC1 已做）、SessionState/EditGate、SessionStore、三领域 Session、真实编译操作、WriteCoordinator、SceneProjection/Hub、SceneRuntime、DetachedView/CodeLease 的必要资源拥有单元全部保留。

如果发现它们中的具体违规，按局部证据处理，不借“重新分配职责”把每个 getter 变成 Processor 方法。每个新增生产类型必须有一个原责任或必要新边界与之对应；没有对应者不新增。
