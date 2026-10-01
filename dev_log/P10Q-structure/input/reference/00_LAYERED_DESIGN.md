# Lux Engine Editor：五层收敛设计

**版本：2026-10-01 / Design 1**  
**性质：目标架构设计，不是已实施代码或阶段放行证明。**  
**当前参考提交：** `LUX-YU/lux-engine@f7c27f9375cbf8dd8af37b30a6027a460de26213`  
**对应 P10Q 实现：** `3c20910d05d635078ef9021a3a3318086a792b8d`

> 目标不是让现有二十个目录外面再套几层，而是让 Editor 只表达它独有的五种责任：编辑约束、作者内容、编辑活动、工作台和产品装配。复用 engine/modules 的实际能力；在需要变化的接点使用抽象，在不存在变化的地方直接组合准确类型。
>
> 本设计替代此前“core / services / presentation / tools / extensions / app”作为最终顶层组织的提案。旧提案适合初步归类，但把技术基础、业务服务和垂直工具混在一张图中，仍不足以说明编译依赖与职责层级。

## 0. 阅读约定与边界

下文分为三类陈述：

- **[现状]**：参考上述提交、已读源码和 P10Q 报告能够确认的事实。
- **[目标]**：本次建议实施的设计。类型名可能是设计名，不代表仓库已经存在该 API。
- **[不变量]**：重组不得破坏的既有行为，按真实测试验证。

P10Q 报告已经表明：项目目录、任务观察、共享编码字节和通用视口已收敛；History、SessionStore、SessionState、RunStore、WriteCoordinator 仍是各自事实的权威。[S1] 本设计利用这些成果，不以新分层为借口再写第二套实现。

此次只读取代表性源码和现有材料；没有独立重跑引擎、SDK、GPU 或所有历史验证。附件中的 C++20 示例是独立的绑定机制演示，不是 Lux 产品源码，也不证明插件 ABI 或完整引擎跨平台通过。

用户已调整验收范围：不补旧 50k 深链至 100 次；Linux 当前无环境，静态可移植性审查继续，但 Linux 构建/运行保持 NOT_RUN，不作为本次结构重组的阻塞。历史 PARTIAL 收据保留，新的范围决定另记，不改写过去成绩。系统 IME 仍独立标记未测。

## 1. 先精确定义依赖方向

### 1.1 “上层不会依赖下层”的可执行含义

本设计将其落实为：

> **高层业务策略不依赖低层具体实现；双方依赖由语义需要者定义的稳定契约。产品装配点是唯一明确知道具体组合的位置。**

不能把这句话写成“上层完全不知道任何下层类型”。例如 ContentStamp、SessionId、冻结源值和正式 SceneRuntime API 都是稳定语义的一部分；禁止引用它们会迫使程序复制同义类型和适配壳。

也不能把传统分层方向颠倒为“基础库可以包含业务窗口，而业务窗口不准使用基础库”。**内层禁止反向知道外层；外层可以使用内层的公开契约，不能穿透实现。** 这与“高层策略不绑定低层机制”是同时成立的两条约束。

### 1.2 四种关系必须分别记录

| 关系 | 例子 | 不能得出的结论 |
|---|---|---|
| 源码依赖 | SaveService 包含保存角色契约 | 不代表必须包含 SceneSession 的实现 |
| 运行时调用 | SaveService 调用某个 Scene 保存角色 | 不代表 SaveService target 必须链接 Scene 作者模型 |
| 对象拥有 | SessionStore 拥有 SceneSession | 不代表 Store 必须知道全部具体 Session 类 |
| 模板实例化/最终链接 | 装配代码实例化编码器并链接实际 codec | 不代表模板策略头应该包含所有 codec 和全部引擎 |

后文统一使用 `A → B` 表示 **A 的源码/构建使用 B 的公开契约**。事件传播、回调方向、拥有关系另行写出，不混用箭头。

### 1.3 一个具体例子

```text
保存策略 ──源码依赖──> 保存角色契约
具体 Scene 保存角色 ─源码依赖──> 保存角色契约 + Scene 作者模型
产品装配 ──源码依赖──> 保存策略 + 具体 Scene 保存角色

运行时：保存策略调用已经注册的角色。
```

禁止再为了这张图创建 `ISceneSaveAdapter -> SceneSaveAdapter -> SceneSaveProvider -> SceneSession` 的纯转发链。具体角色若存在，必须真正负责源格式、快照和基线采用等领域映射；没有独立语义的中间对象应合并。

## 2. 与 modules / engine 的语义分界

### 2.1 三个仓库顶层分别回答什么

| 顶层 | 回答的问题 | 典型责任 |
|---|---|---|
| `modules/` | 有哪些可复用的基础和运行能力？ | LuxObject、信号、容器使用、元信息、UI 基元、资产表示、图、渲染后端、平台机制 |
| `engine/` | 如何装配并驱动可运行内容和有限工作？ | SceneRuntime、Simulation/World 集成、Process 执行、资产加载、正式编译工具链、运行插件加载 |
| `editor/` | 用户如何修改、检查、保存、运行和恢复作者内容？ | 工作副本、编辑历史、用户意图、基线采用、工作台、产品流程 |

这是职责分界，不按“CPU/GPU”“同步/异步”机械切分。作者模型可以使用现有 CPU Registry、图和 schema；Editor 不因此取得重做 ECS 的理由。

### 2.2 明确不在 Editor 重复建设的能力

| 已有能力 | Editor 的正确使用 | Editor 中禁止再造 |
|---|---|---|
| LuxObject / TSignal / Connection | 通知已提交事实、控件事件、绑定接收者寿命 | EditorObject、EditorEventBus、另一套订阅表 |
| ExecutionRuntime / Task / TaskScope | 编码、解码、编译、文件工作和可靠完成运输 | EditorExecutor、私有线程池、通用 CompletionManager |
| SceneRuntime / 实例 lease / retirement | 发起运行、观察、控制、消费实例结果 | 第二 SceneRuntime、第二驱动循环、另一个实例回收器 |
| RenderRuntime / RenderResources | 视口输出、资源使用权、后端完成与退休 | EditorRenderer、编辑器 GPU fence/资源生命周期副本 |
| engine/toolchain | 请求正式 Material/Flow 编译，固定产物重试 | 编辑器私有编译器、临时 shell 拼接的第二链接器 |
| engine/project/plugins | 装载代码、核对实际运行依赖 | Editor 再实现一个平台动态库装载系统 |
| asset VFS、codec、文件发布基元 | 作者源编码、目标版本与发布语义 | 第二资产格式、第二 VFS 或通用文件系统框架 |
| imgui-node-editor | 由 GraphCanvas 统一接入 Lux UI | 第二节点画布引擎、第二图数据模型或撤销栈 |
| lux-cxx | 按地址稳定性、密度、容量和测量选择容器/字节/回调 | 重写已有容器，或为“复用率”强行替换标准库 |

[S2–S4] 支持既有 Process、Scene 和对象设施的边界；这些资料不意味着当前所有调用都已经完美。设计上的目标是继续直接使用这些能力，而不是在 Editor 镜像出同名技术层。

### 2.3 技术机制和 Editor 语义不是同一事实

`TaskId` 不等于 `SaveId`；`Task.finished` 不等于文件已经发布；Scene 已退休不等于用户已确认单步结果；Renderer 接受 program 不等于 GPU 已结束。

因此，本设计不删掉确有事实差异的 Save、Run、Preview 记录。删减对象的依据是“没有独立事实/不变量/寿命”，不是“底层已经有一个 ID”。

## 3. 目标：五层，而不是六组混合目录

```text
editor/
├── editing/       # E0：编辑公共约束与会话容器
├── authoring/     # E1：具体作者内容及纯编辑语义
├── activities/    # E2：用户编辑活动、在途操作及结果
├── workbench/     # E3：交互、具体视图、编辑器桌面呈现
├── application/   # E4：产品组合、扩展安装、入口与总生命周期
└── tests/         # 跨层验收，不是业务层
```

层号表示从稳定内核向产品外壳展开的顺序，不表示所有调用都必须逐层转发。

| 层 | 语义核心 | 变化原因 | 允许的主要依赖 |
|---|---|---|---|
| E0 editing | “编辑一份工作副本”共有的不变量 | 编辑身份、历史、准入和保存基线契约变化 | 既有基础设施和纯身份/值 |
| E1 authoring | “这是什么内容，什么修改合法” | Scene/Material/Flow/项目/布局语义变化 | E0 + engine/modules 的纯描述、图、schema、codec |
| E2 activities | “用户发起什么工作，结果是什么” | 保存、打开、运行、编译、恢复政策变化 | E0/E1 公共契约 + 原引擎技术能力 |
| E3 workbench | “如何交互、呈现和发出请求” | 控件、工作台、视口和交互体验变化 | E0/E1/E2 公共契约 + 原 UI/渲染/输入设施 |
| E4 application | “本产品选哪些实现，何时建立和结束” | 产品组合、扩展集、启动和退出策略变化 | 所有层的正式契约及选择的具体构造入口 |

不再使用 Editor 顶层 `core/domain/process/runtime/platform/adapters/ports/managers/common`。理由不是这些词普遍错误，而是此项目已经在 engine/modules 有明确技术语义；在 Editor 再使用一遍容易形成假想的平行引擎。

## 4. 完整目录形态

下列是职责位置，不是“每一行一个库”。二级子目录只有形成完整特性边界时才有独立 include/src/test 与 CMake；同一 target 内的几个协作文件放在一起。

```text
editor/
├── editing/
│   ├── include/lux/engine/editor/{editing,sessions,contracts}/
│   ├── src/                 # history、session 管理、准入实现
│   ├── test/
│   └── CMakeLists.txt
│
├── authoring/
│   ├── scene/               # SceneSession/SceneSource/领域编辑与快照
│   ├── material/            # MaterialSession/领域图编辑与快照
│   ├── flow/                # FlowSession/变量、签名、连接与快照
│   ├── project/             # ProjectManifest/ProjectCatalogModel/目录值
│   ├── layout/              # DockLayout/Recovery/Preferences/纯 LayoutPlan
│   └── CMakeLists.txt
│
├── activities/
│   ├── persistence/         # SaveService/WriteCoordinator/SaveExecution/文件发布
│   ├── project/             # 项目打开、存储更新、导入与构建请求
│   ├── scene/               # RunStore、启动/停止、作者投影活动
│   ├── material/            # 编译记录、预览采用、产物发布
│   ├── flow/                # 编译/链接尝试与固定产物发布
│   ├── workspace/           # WorkspaceStore、迁移、内容恢复计划；不挂载 UI
│   ├── tasks/               # TaskMonitor：观察 Process，不拥有第二份任务
│   ├── commands/            # P11 的命令查询/执行、不可变注册
│   └── CMakeLists.txt
│
├── workbench/
│   ├── desktop/             # DesktopShell、ViewHost、输入和 UI 呈现接线
│   ├── viewport/            # 视口本地相机/输出/输入/关闭资源
│   ├── widgets/             # GraphCanvas、TreeRows、NodeCanvas
│   ├── scene/               # SceneInteraction、SceneView、Inspector 等
│   ├── material/            # MaterialInteraction、MaterialView/草稿
│   ├── flow/                # FlowInteraction、FlowView/草稿
│   ├── project/             # ProjectView、AssetPicker
│   ├── tasks/               # TaskView
│   ├── settings/            # 偏好和扩展配置 UI
│   └── CMakeLists.txt
│
├── application/
│   ├── src/                 # 启动、组合、生命周期泵与跨 UI/内容的顶层用例
│   ├── extensions/          # 内置扩展安装、外部加载接线，不是全局服务表
│   ├── launch/              # launcher/main、Editor 进程入口
│   ├── test/
│   └── CMakeLists.txt
└── tests/
    ├── architecture/
    ├── installed/
    └── integration/
```

### 4.1 为什么不继续保留顶层 tools

旧方案的 `tools/scene` 同时覆盖模型、运行、交互、UI，属于垂直功能包；顶层 persistence、desktop 又是水平服务层。两种组织方式并列，无法给根目录统一含义。

本目标优先选择真正的水平分层。Scene 在三个层出现，分别是作者内容、编辑活动、工作台工具，**不是三份 Scene 实现**。层内仍按领域聚合，避免把所有 model 放进一个巨型文件夹。

这是一项架构选择，不声称垂直切片普遍不如分层。此处用户明确要求与整个引擎一致的分层和明确依赖，因此不再同时维持两套顶层分类。

### 4.2 公开命名与物理目录不必重复

可以继续使用 `lux::editor::scene::SceneSession` 和 `lux/engine/editor/scene/SceneSession.hpp`，不必产生 `lux::editor::authoring::scene::model::SceneSession`。

逻辑 include 保持一个真实文件、一个真实定义，不是兼容桥。移动源码不要求顺便改所有 schema、资产格式、公开身份或命名空间。按实际职责纠正误名，但不对正确名称机械重命名。

### 4.3 不建空目录，不给每个名词一个头

未来 P11/P12 还未实现的目录仅在代码实际加入时创建。紧密相关的小型值、概念和函数可以放在一个语义头中。禁止新增 Concepts.hpp、Managers.hpp、Common.hpp 这种跨全 Editor 的杂物集合。

## 5. E0 editing：编辑不变量，不是第二个基础库

### 5.1 保留与组合

E0 保留已有 `EditHistory`、`SessionState`、`SessionStore`、`IEditSession`、代际 key、ContentStamp、关闭许可和保存 checkpoint。把原 contracts 中真正属于编辑共同语义的内容归到此处；**不照搬全部 contracts 文件**。

```text
SessionStore
  └── unique_ptr<IEditSession>
        ├── SceneSession
        ├── MaterialSession
        └── FlowSession

每个具体 Session 组合：
  作者源 + 自己的 EditHistory 实例 + SessionState
```

`IEditSession` 的少量运行时虚函数有真实理由：Store 必须持有同一容器里的不同会话，并预留 P11 的运行时扩展。它不能扩展成所有工具功能的公共大接口。[S5]

### 5.2 禁止 E0 承担

不编码 Scene/Material/Flow；不创建 Pane；不写文件；不编译；不启动运行实例；不执行“退出时弹框”；不提供获取所有服务的 Context。

CodeLease 沿用当前唯一正式代码保活表示。若底层已能直接提供同一个所有权类型，就直接消费；不能在 E0 再定义 EditorCodeLease 对它逐层转发。身份生成的进程唯一状态必须继续符合跨 DLL 要求。

### 5.3 ViewInfo 不再作为默认公共杂物

纯 `ViewId/ViewRestoreKey/ViewTypeId` 可放在最小共有身份契约；Pane/Root/活动绑定等视图语义属于 workbench。纯布局规划使用 authoring/layout 自己定义的只读布局观察值，workbench 输出该值。

这个投影是瞬时输入，不保存第二个活动视图目录。若现有 ViewInfo 本来就全部是规划所需的纯值，则直接保留唯一值定义，不为了目录划分再复制一份。

## 6. E1 authoring：内容语义与纯验证

### 6.1 每个领域独立，而不是万能 Document

| 领域 | 唯一可写事实 | 必须复用 | 不能持有 |
|---|---|---|---|
| Scene | SceneSource 的作者组件与结构 | 原 CPU Registry/schema/WorldObjectId/场景描述 | SceneRuntime、Pane、GPU 输出、RunSession |
| Material | MaterialSource 中的作者图 | 原 MaterialGraph、节点/Pin 身份和 codec | 编译 Task、预览实例、文件发布记录 |
| Flow | FlowAuthoringSource、FlowGraph | 原变量、签名、类型环境与 codec | linker、脚本运行实例、视图选择 |
| Project | Manifest 与唯一目录版本 | 正式 AssetId/AssetReference/目录索引 | Pane、平台窗口、ProjectBuilder 的异步任务 |
| Layout | DockLayout、Preferences、Recovery 等值 | 稳定 LayoutId/恢复键 | 活动 Root、打开资产回调、运行会话 |

现有作者模型是正确基础。[S1,S8] 不将三个作者源统一为 `std::any document`，不让 SceneSession 成为 MaterialSession 的基类。

### 6.2 公共方法表达原子性，私有方法利用已建立不变量

公共 `apply(expected, edits)` 负责原内容版本、gate、领域约束和一次历史提交；私有准备算法可直接使用已验证且未越过回调边界的引用。

不能把 prepare 中的验证删掉，转而要求所有 UI 调用者“保证正确”。也不能每进入一个无回调 helper 就重新进行整个 Store 查找和全量验证。

快照复制、数据编码和 schema 回调可能调用插件代码；稳定读取和输入清理必须保持原准入作用域。`const`、concept 和 `shared_ptr<const T>` 都不能单独证明深层不可变性。

### 6.3 纯布局计划不依赖 workbench

`planLayout(layout, inventory)` 只消费纯值或满足局部概念的只读观察范围，输出创建/复用/摆放的计划。它不创建视图、不解析 opaque 打开文件，也不读 active Root。

workbench 负责生成 inventory 和真正准备/提交视图挂载；activities/workspace 负责文件、迁移和内容恢复请求，不认识 ViewHost；application 组合二者的结果。数据可以从外层流入内层，源码依赖不反向。

## 7. E2 activities：拥有用户活动，不拥有技术执行器

### 7.1 为什么用 activities 而不是 services/process

这里处理的是用户可辨认的工作：保存、编译、运行、打开、恢复、退出准备。它们可能跨帧，有业务身份和终态，也可能只是一次无状态查询。

`activities` 不是通用工作流引擎，不要求所有类命名为 Activity。已有准确名称如 SaveService、RunStore、WriteCoordinator 可以保留；无状态调用继续用自由函数。

### 7.2 内部职责

| 活动 | 拥有或维护的事实 | 技术执行/重资源归属 |
|---|---|---|
| 保存 | 请求、冻结版本、Published/Unknown、采用结果、确认 | 原 Process + 同一个 WriteCoordinator + 真实文件后端 |
| 编译 | 内容/config/env/target key、编译/链接尝试、固定产物 | engine/toolchain + Process |
| 预览 | 期望版本、候选/已采用关系、过期诊断 | 原 SceneRuntime/RenderResources/RenderRuntime |
| Run | 来源、启动/停止请求、对原退休结果的使用 | SceneRuntime 的实例与单步结果权威 |
| 项目操作 | 导入、项目更改、目录发布和构建请求 | 原资产读取、文件与构建机制 |
| 工作区操作 | 布局文件、迁移 marker、内容恢复执行进度 | 原协调器；纯 LayoutPlan 在 E1；活动 UI 应用在 E3 |
| 任务观察 | observer 注册、修订提示、只读缓存 | ExecutionRuntime 保持唯一任务事实 |
| 命令 | 接受的目标、注册版本、执行回执 | 被调用的具体活动，不复制其状态机 |

### 7.3 E2 内部允许分 API 和实现，但不再增加 Port 层

需要让上层消费而隐藏技术实现时，在同一责任模块提供正常公共头和私有实现：

```text
activities/persistence/include/.../SaveService.hpp
activities/persistence/src/SaveService.cpp
activities/persistence/src/SaveExecution.cpp
activities/persistence/src/FileArtifactStore.cpp
```

是否分 target 由闭包决定：纯协调与 Process 接线目前就有分离理由。模块内的文件后端可以保留真实 IArtifactStore 契约；不为每个具体类配一个 I 类，也不建立 `port/adapter/implementation` 三重目录。

通用 SaveService 不包含三种 Session 的具体头；领域保存角色放在相应活动实现，消费作者模型与保存契约。角色包含格式映射和采用语义，不能只是复制三行无意义转发。

### 7.4 用户选择以数据往返，而不是 E2 调 UI

关闭业务产生 `ClosePlan/UnsavedItems` 一类拥有型结果；工作台显示并返回带原身份/版本的决定；关闭业务验证后推进。E2 不依赖 Dialog、Pane、ImGui 或 ViewHost 的具体实现。

这不是要求新增一套专用消息总线。使用既有活动记录、命令和 LuxObject 通知即可；每项决定与原内容版本绑定。

## 8. E3 workbench：交互与显示，不再拥有业务工作

### 8.1 两种内部边界

- **可复用 UI 基础**：desktop、viewport、widgets、通用 View 契约。不得依赖 SceneSession、MaterialSession、FlowSession。
- **领域工具 UI**：scene、material、flow、project、tasks、settings。依赖对应作者公开模型和活动契约，不依赖其他工具 UI 的实现。

二者属于同一工作台层，但不是同一个 target。用一次真实 viewport 提取避免 MaterialView 链接 Scene Inspector、创建工具和生成器。

### 8.2 interaction 放在工作台层，不代表必须链接 UI

SceneInteraction、MaterialInteraction、FlowInteraction 表达用户手势/选择，因此语义属于 workbench。它们仍是可无窗口执行的 CPU 目标；不会仅因物理目录改变而新增 ImGui、Root 或 Vulkan 依赖。

一层不等于一库。将 interaction 编进 GUI 大 target，会丢掉现有纯 CPU 验证能力，明确禁止。

### 8.3 减少重复的展示状态

每个图工具最多保留三种不同用途的值：

1. 已展示的不可变快照与 stamp；
2. 用户可丢弃的草稿（value + based_on）；
3. 已接纳、尚未提交的有界输入（payload + based_on + phase）。

它们不是作者源的三份权威副本。display 刷新不得修改 draft/request 的来源戳；BUSY 保留相同 owner 和 phase；STALE 进入明确的恢复路径。

`deliverInput` 保持小型局部算法，可用 concept 把两种领域交互的共同步骤约束清楚；不能升级为支持所有 View 的通用 WorkflowManager。相同的是交付规则，不同的是领域 edits、验证与诊断。

### 8.4 ViewHost 的唯一责任

ViewHost 独占顶层 DetachedView，管理视图身份、准备/挂载、关闭记录；Root 负责活动登记、焦点与路由。活动内存、Task 和运行实例不属于 Host。

关闭视图不等于关闭作者内容，不等于取消应用拥有的保存/编译，不等于停止 Run。隐藏、关闭、解绑有明确不同语义。

## 9. E4 application：知道具体组合，但不包办业务

### 9.1 Composition Root 的例外是显式且局部的

这是唯一允许同时 include 具体文件后端、具体活动服务、具体工具视图构造入口的位置。它绑定实际实现、初始化顺序与退出顺序。

```text
EditorApplication
  ├── 借用/组合原 EngineContext 与正式运行设施
  ├── SessionStore（E0）
  ├── 活动服务与唯一 WriteCoordinator（E2）
  ├── ProjectStorage / Catalog、TaskMonitor
  ├── 不可变扩展安装快照
  └── DesktopShell / ViewHost（E3）
```

这张图描述总拥有关系，不要求所有成员都挤进一个巨型 Impl。固定、完整、寿命一致的服务直接成员组合；失败可选能力以明确创建结果或状态表达；不是所有成员 `unique_ptr` 然后到处判空。

### 9.2 应用层不得成为新的 EditorContext

禁止提供 `getService<T>()`、`void* get(name)`、任意下转型或遍历查找服务。每个消费者构造时只接收它需要的正式能力。

SaveAll、关闭内容的未保存决定、内容恢复等不需要 UI 的政策放在 E2。真正跨越内容、工作台和平台退出的顶层用例（如 ExitEditor、OpenAndShow、RestoreWorkbench）属于 E4，可各自实现为小操作/函数，不挤进 EditorApplication 巨型 Impl。它们只拥有组合进度和总结果，不再次保存 Save/Run/ViewHost 的同一状态。application 不是必须没有逻辑；它必须没有下层事实的重复权威。

### 9.3 扩展不是第六层

扩展契约属于需要它的语义层：

- 会话生命期与历史角色：E0；
- 源格式/作者扩展描述：E1 或既有 schema 体系；
- 保存角色、命令与活动工厂：E2；
- 视图工厂、控件扩展：E3。

`application/extensions` 只负责组合这些注册、调用原插件装载、安装内置贡献和退出时保持代码寿命。整批恢复所需的同一扩展版本可以由组合快照固定；不创建一个人人依赖的万能 ExtensionManager。

## 10. 依赖矩阵：允许边与禁止边

`公开`指正常声明、值和语义契约；不允许通过 public include 暴露私有实现来规避。

| 消费者 | editing | authoring | activities API | activities implementation | workbench | application |
|---|---:|---:|---:|---:|---:|---:|
| editing | 本层有向依赖 | 禁止 | 禁止 | 禁止 | 禁止 | 禁止 |
| authoring | 允许 | 领域内；跨域须有真实共享语义 | 禁止 | 禁止 | 禁止 | 禁止 |
| activities 策略/API | 允许 | 需要的值/协议 | 同层显式 DAG | 禁止反向拉入后端 | 禁止 | 禁止 |
| activities 实现 | 允许 | 对应正式模型 | 允许 | 显式 DAG，不能形成互相驱动 | 禁止 | 禁止 |
| workbench 通用 UI | 仅必要共同值 | 仅纯布局值（若需要）；禁止具体作者模型 | 仅必要桌面命令契约 | 禁止 | 本层明确 UI 依赖 | 禁止 |
| workbench 具体工具 | 允许 | 对应领域公开能力 | 允许 | 禁止技术后端头/私有状态 | 共用 widgets/viewport；禁止另一个工具 UI | 禁止 |
| application 装配 | 允许 | 允许 | 允许 | 仅构造/接线点允许 | 允许 | 本层 |

附加规则：

- engine/modules 不依赖任何 Editor target，PLAYER 闭包不出现 Editor。
- 作者模型不得通过间接头或 INTERFACE_LINK_LIBRARIES 引入 Process、SceneRuntime、compiler、GUI。
- 模板实例化依赖也属于依赖：要求编译源码和生成的依赖文件核对，不只扫描 #include 字符串。
- workbench 实现渲染时可以调用原 RenderRuntime 的正式 API；禁止直接依赖 Vulkan 私有后端和使用其内部结构。
- 同层也需要 DAG。A/B 若互相依赖，优先移走共同语义或调整 owner；不能用回调函数表隐藏环。

## 11. 抽象机制的选择：不是“全 virtual”与“全 template”二选一

### 11.1 六种机制各有适用范围

| 情况 | 首选机制 | 例子 | 不该做的事 |
|---|---|---|---|
| 一个稳定的正式能力，不需要替换实现 | 正常类 API + private/PImpl | SessionStore、WriteCoordinator | 为它再写 IStore 和转发 StoreAdapter |
| 多种编译期已知类型共享同一个算法 | C++20 concept + 小函数模板 | 快照编码、输入阶段推进、只读布局算法 | 整个 EditorApplication 模板化 |
| 类型集合封闭，运行时选择 | `std::variant` + 穷尽 visit | Unbound/Authoring/Running 绑定 | any + type tag + downcast |
| 类型集合开放，必须异构拥有或运行时加载 | 窄 virtual 或一次 owning type erasure | IEditSession、ISaveSource、插件视图工厂 | 先 virtual 再另包多层 function table |
| 已提交事实/控件事件，一对多观察 | LuxObject 信号 + Connection | catalogChanged、任务修订、closeRequested | 手工接收者列表、Signal 代替 Result |
| 当前栈内的小行为借用 | 模板 callable 或 lux-cxx function_ref | 即时字段访问 | 把 function_ref 保存在下一帧任务中 |

### 11.2 静态绑定的边界

concept 只声明一个算法所需的能力，不强迫类型继承特定基类。类型是否满足表达式、返回类型、noexcept 可以由编译器验证；预算准确性、没有重入、快照深层拥有、原子性和唯一派发不是编译器自动证明的事实。[L1,L2]

一个 concrete class 也能提供良好的抽象。`SceneSession::apply` 隐藏数据结构和领域验证，就是稳定抽象，不必先改成 `ISceneSession` 才算面向抽象。

### 11.3 动态绑定的边界

Store 必须管理在运行时才知道的不同会话；菜单必须执行运行时注册的命令；外部插件可能增加未知视图。这里不能仅靠 concept 解决，因为 concept 不是运行时对象，也不提供 ABI 或统一存储。

优先保留当前已经合适的窄虚接口。[S5,S6] 不为消灭 vtable 而发明更复杂的任何对象包装。vcall 在冷路径可接受；真正频繁的内部算法依然可以静态实例化。

### 11.4 两者如何结合而不重复包装

```text
编译时：SceneSnapshot + SceneCodec 满足 FrozenEncoder
        MaterialSnapshot + MaterialCodec 满足 FrozenEncoder

注册/任务建立处：
  选择具体模板实例，产生一个既有 OwnedEncodeJob

运行时：
  SaveService 只持有该任务/保存角色的统一契约
  Task 在原 Process 执行
```

允许在这个真实异构边界**擦除一次**。同一个对象不能再经历 CodecPort → CodecAdapter → ErasedCodec → ICodec → Job 五层包装。

## 12. C++20 concept 的具体设计

### 12.1 冻结编码能力

以下是**目标签名示意，不声称当前每个类已有这些名称**。实际实施复用现有 EncodedArtifact、PersistenceResult、OwnedEncodeJob，禁止把示意别名抄成第二套生产结果体系。

```cpp
template<class Codec, class Snapshot>
concept FrozenEncoder =
    std::is_nothrow_destructible_v<Snapshot> &&
    requires(const Codec& codec,
             const Snapshot& snapshot,
             std::size_t limit,
             std::stop_token stop)
    {
        { snapshot.content() } noexcept
            -> std::same_as<ContentStamp>;
        { codec.encode(snapshot, limit, stop) }
            -> std::same_as<PersistenceResult<EncodedArtifact>>;
    };
```

**语义律，不能由上面语法推导：**

1. snapshot 拥有所需数据或保有合法 immutable owner；不借用 live Session、Pane 或 Registry。
2. codec 不修改作者模型，不查当前焦点，不采用 checkpoint。
3. 返回 bytes 不超过 limit，失败和取消区分。
4. 动态代码和 deleter 在输入、任务、结果析构期间有效。
5. 整个工作仍经 Process 准入、执行和交付，不自行创建线程。

该 concept 应和实际通用编码实现放在同一个相关头中。若只有一个调用处没有共享算法，不独立引入 concept。

### 12.2 输入交付能力

两种图工具确实复用小型 deliverInput。适合约束的是“在同一原始 stamp 下推进现有交互”，而不是泛化 Material/Flow 的图语义。

接口要分别表达 begin/preview/commit/cancel 的返回值与错误分类；不把所有领域 Result 压成 bool。可通过既有 error 分类自由函数提取 `retryable/busy/stale`，原错误仍保留。

模板只承担共有阶段推进；payload、queue、based_on、source 仍由具体 View/interaction 拥有。没有第二队列、没有通用 ActivityScheduler。

### 12.3 无状态算法概念比整服务模板更优先

例如全图父关系检查可约束一个只读 parent-index range，而不模板化 SceneSession 或 WorkspaceStore。已有线性算法的语义保持；由场景和布局负责在各自稳定读取中构造输入。

若两个领域的“非法引用”策略不同，不能为了共享而统一错误判定。可只复用遍历核，领域前后验证保持独立。

### 12.4 不建立概念森林

禁止只有 `requires { t.run(); }` 却无语义说明的空概念；禁止每个类自动生成一个 C 前缀 concept；禁止 `TEditor<TStore,TClock,TView,TTask,TFile,...>` 无限制传播；禁止为了 mock 给全部生产类模板参数。

已知两三种真实实现用小型模板很合适；需要独立编译、运行时替换或 code lease 的边界用明确动态接口。标准库/lux-cxx 已有概念和 traits 能表达的，不重命名一份。

## 13. 模板与链接：实例化放在哪里

### 13.1 三个区域

- **契约定义**：消费者所属语义模块的公共或内部相关头。
- **通用算法**：同模块的小型模板实现；不能 include 具体后端。
- **实例化与接线**：提供者实现 CPP 或 application 的明确组合单元，包含选中的具体类型。

built-in 组合稳定且编译成本明显时，可以显式实例化。真正供外部源码扩展的模板在 SDK 保留定义，不用 extern template 禁止未知扩展。

### 13.2 静态绑定不是“完全没有实现依赖”

模板实例化单元需要看到具体类型及有关定义，最终二进制也需要链接真实实现。得到的是：**策略源代码可独立于具体后端定义，替换只影响组合/实例化点**，不是虚构“零依赖”。

同样，PRIVATE 链接不等于不存在依赖；静态库依赖还要在最终链接解析。源层、目标闭包和二进制装载均要检查。[L3]

## 14. Owner、状态和错误的收敛

### 14.1 唯一事实表

| 事实 | owner | 允许的投影/观察 |
|---|---|---|
| 作者内容和历史 | 具体 Session，SessionStore 独占 | 冻结源、UI 显示快照、草稿 |
| 保存基线/绑定/准入 | SessionState | SessionInfo，不复制 authority |
| 文件同目标发布顺序 | 一个 WriteCoordinator | 发布回执/状态查询 |
| 有限任务执行 | ExecutionRuntime | TaskMonitor snapshot/修订 |
| 实际实例与单步终态 | 原 SceneRuntime/lifetime | RunStore 使用退休结果 |
| 运行用户意图 | RunStore/RunSession | RunInspectAccess、运行视图 |
| 预览版本和采用 | 原 PreviewStore/ProjectionHub | Viewport 输出与陈旧标记 |
| 顶层 Pane owner | ViewHost | Root 的非拥有登记、ViewInfo |
| 项目目录数组/索引/修订 | ProjectCatalogModel | 同版本共享 Snapshot |
| 扩展注册版本 | 对应只读注册快照 | 命令/工厂 handle 持同一版本与代码寿命 |

### 14.2 状态应当排除非法组合，但不一律 variant

互斥绑定适合 variant；小型单步状态枚举 + 有界记录可继续保留；带生命周期的候选使用 move-only RAII。不要把业务上本来正交的状态硬塞进一个巨大 State 枚举，也不为每个布尔值创建类型。

### 14.3 错误不是为了跨层而反复翻译

同步公开入口保留自己的领域错误。上层只在真正需要统一呈现时形成一次拥有型诊断，不在每层套 `variant<variant<...>>`。

BUSY、STALE、CLOSED、权限、缺失、未知发布含义不同。界面可以把错误分类为可重试/需用户处理，但原始事实不能丢；没有一个万能 BUSY 可以代理所有关闭失败。

### 14.4 检查有效期

在工厂或准入处保证必需依赖有效；受控、连续、没有回调的内部 helper 直接用引用。

必须重新检查的位置：跨 frame、跨异步、跨外部回调、注册撤销、重绑定、代际复用、IO 发布、资源退休。concept 不能替代这些运行时事实。

## 15. 五条端到端调用链

### 15.1 修改 Material 常量

```text
GraphCanvas / 属性控件（workbench/widgets）
  → MaterialView 固定 payload + based_on（workbench/material）
  → MaterialInteraction 预览/提交（workbench/material，CPU）
  → MaterialSession 原子 apply（authoring/material）
  → EditHistory/SessionState（editing）
  → 原模型变化被预览活动与其他视图观察
```

任何一步都不需要 EditorContext 或任意服务查找。模型不回调具体窗口。

### 15.2 保存后继续编辑

```text
用户 Save / CommandInvocation 固定目标
  → SaveService 准入并预留目标票据（activities）
  → 具体源角色在作者 gate 内捕获
  → OwnedEncodeJob 进入 Process
  → 完成被可靠接收，WriteCoordinator 决定发布顺序
  → FileArtifactStore / 原文件基元发布
  → Published 与 checkpoint adoption 分开记录
  → LuxObject 提示工作台刷新
```

S10 捕获后可以编辑成 S12；S10 发布不令 S12 clean。窗口关闭不丢保存责任。

### 15.3 运行与停止

```text
工作台发 StartRun
  → 活动固定作者快照
  → Process 做有限准备
  → owner 安全点用原 SceneRuntime 创建实例
  → RunStore 发布 RunId
  → 统一宿主驱动 Runtime
  → Stop 请求 → Runtime 实际退休
  → 单步终态继续可查询直到显式确认
```

RunStore 不在内部再调用第二轮 driveFrame。停止不隐式写回作者源。

### 15.4 应用布局

```text
工作区活动读取新布局值
  → authoring/layout 纯验证、生成计划
  → application 将拥有型计划交给工作台
  → workbench/Desktop 准备需要的 DetachedView、路由与槽位
  → 安全点提交摆放/视图关联
  → 提交后的通知与诊断
```

内容恢复来自独立 manifest 经 OpenAsset 活动，不由布局 opaque 或工厂偷偷打开全部旧内容。E2 不调用 E3 的 ViewHost；若一个操作必须同时安排内容与视图，组合部分位于 E4，不能把 IViewHost 头反向拉入 activities。

### 15.5 扩展提供新工具

```text
编译期：扩展作者用 concept 检查具体工厂/codec
运行期：application 调原插件加载设施，安装不可变注册
       → workbench/activities 持相应窄 handle
       → 未知实现通过一次动态边界调用
       → 代码 owner 直到实例/回调/任务/错误值全部退出才释放
```

concept 不是插件 ABI。没有必要把全部 Editor 编成 DLL；也不能让插件和 exe 各自拥有一套唯一身份计数器。

## 16. CMake 和二进制组织

### 16.1 五个目录不是五个库

根 CMake 只负责这五个责任层及测试的配置次序。层 CMake 负责层内真实 target；工具实例化按其目标闭包显式登记。

```cmake
add_subdirectory(editing)
add_subdirectory(authoring)
add_subdirectory(activities)
add_subdirectory(workbench)
add_subdirectory(application)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

这是目标形态；迁移过程中原 root 的受控兼容配置可以暂留到消费者切换，不创建第二套完整 root 构建。

### 16.2 target 只为真实闭包分割

值得保留的边界：三种 CPU 作者模型、CPU interaction、编译工具链、GPU 工作台、纯协调与执行接线、插件 ABI。无需为 selection、snapshot、ticket、factory、concept 各建库。

默认第一方产品内部 STATIC；纯头 INTERFACE；OBJECT 仅在确有复用对象文件需要时使用。不要为减少文件数将所有 Editor 编入一个巨型库，导致任何模型测试都要求 LLVM/Vulkan/Windows UI。

### 16.3 共享状态例外

同进程多个 DLL 确实共享的身份分配、反射或宿主服务，其 owner 不能因静态链接在每个 DSO 各来一份。可以保留现有最小共享边界，或者让插件借用宿主显式传入的状态；两者择一，不建立隐式 singleton 副本。

本阶段不单独发明新的插件二进制协议。P11 按这一规则完成真实扩展 ABI 与代码寿命；P12 完成唯一产品切换。

### 16.4 SDK 不等于全部源码都是 API

只安装实际源码消费者和扩展需要的契约；内部 helper、测试访问、PImpl 头、构建过程产物不导出。模型 SDK 可以独立消费，但不承诺用户必须用原产品 exe。

不得以删除 SDK 回归来掩盖依赖；也不必让几十个内部概念各自维护一个包版本。收拢 package 时修改全部消费者，不留下旧别名包转发。

## 17. 做减法的明确清单

### 应退出的结构

- 顶层 `contracts/services/core/tools` 并列的混合分类，按新责任位置归并；不新增永久 adapters/ports。
- 旧 Context、PaneManager、三大旧 Editor 与 transition 按真实消费者切换在 P12 删除。
- 无状态 Operation 类、单别名头、只有一跳转发的 Provider/Adapter/Controller。
- 全部模块各自定义“owner 指针 + 函数表”来逃避类型和依赖检查。
- 工作台通过任务 UI 包使用 TaskMonitor；Material 通过 Scene UI 包使用通用 viewport。
- 重复的 current/dirty/history/runtime/task catalog authority。
- 根 CMake 交错配置每种工具 model/persistence/UI 的长清单。
- 根 README 默认描述旧 Editor 窗口拥有内容与历史，而新代码按相反规则实现的矛盾。

### 应保留的少量结构

- 真实生命周期的身份、许可、冻结输入和 RAII owner。
- 真正有业务终态的 Save/Run/Compile 记录。
- 开放异构的 IEditSession、ISaveSource 以及合适的工厂抽象。
- 必要 CPU/GPU/toolchain/SDK 构建边界。
- 原文件格式读取和明确迁移；数据兼容不是旧代码体系兼容。
- 实际共享资源和晚到完成的责任，不为短代码牺牲。

原则：**整体应减少独立协议、拥有者和纯转发跳数。目录重排后若这些数量完全没变，必须说明到底改善了什么；不能以根目录变少作为充分结果。**

## 18. 性能、容器和可移植性

### 18.1 性能不因分层自动变好

P10Q 已经改进深链验证、任务稳定帧目录、共享产物字节和画布身份有界性。[S1] 保留这些行为，不补无意义的旧慢算法样本。

通用循环模板不能引入完整快照重复捕获；工作台读取版本应廉价；不可变共享数组比每窗复制好，但不是维护第二份目录。局部小集合扫描可以保留，除非实测或输入上限证明需要索引。

### 18.2 lux-cxx 是工具，不是替换配额

- 固定地址要求才考虑 StableSlotMap；它不能替代防止回调期间删除的保护。
- SmallVector 只在元素分布和移动约束适合时采用；不把 SBO 当零分配证明。
- SparseSet 不接收任意 UUID/hash 作为稠密下标。
- SharedBytes 保留真实底层 owner；不能从短寿命 span 捏造拥有关系。
- function_ref 只借当前栈；需要跨帧则 owning callable 或现有正式角色。

### 18.3 当前平台范围

Windows 实际构建、安装和相关 UI/GPU 回归继续。Linux 当前只做源码和构建规则审查，明确 NOT_RUN；不要求为此次分层新建 Linux 环境。语法审查不等于跨平台通过。

保持 C++20，不引入 std::expected、std::move_only_function 等 C++23 接口作为无条件依赖。生产 Result 和 owning callable 复用 lux-cxx 当前已验证版本；示例可用独立简化类型，但不能复制进产品成为第二套 expected。

## 19. 迁移次序与阶段关系

详细路径表在 `01_MIGRATION_AND_ACCEPTANCE.md`。此处只给依赖次序：

1. 固定本设计、当前 SHA 和新的验收范围；读取实际消费者，不覆盖用户改动。
2. 收拢 E0/E1，保持 CPU 模型与公共逻辑 include，不动磁盘身份。
3. 收拢 E2，纠正任务观察、文件和运行/编译的真正归属；明确 API/实现依赖。
4. 收拢 E3，移走旧工具 model/activity，只保留交互/显示；通用 UI 不依赖具体工具。
5. 收拢 E4 构建组合和文档，删除已无消费者的旧 root/别名/包。
6. 只在实际共用算法上引入 concept；动态开放边界保留一次，核对实例化闭包。
7. 回归与审阅后进入 P11；P11/P12 按本层次写新代码，P12 清理全部到期旧体系。

不要把这次目标结构转换成一个新的 P10R/P14 产品框架。它是 P10Q 内的结构设计与收敛，以及 P11–P13 的持续约束。

## 20. 验收标准

验收不是“所有文件都移动成功”。必须回答：

- 能否只构建/链接任一种 authoring 模型而不配置 Editor 工作台或编译后端？
- 高层保存策略能否在不 include 三种具体 Session 和文件后端的条件下编译？
- concept 负例是否命中契约错误，而不是缺头文件？
- 两个真实编码/交付类型能否复用同一小算法，且没有模板化整个服务树？
- 开放插件/Session 是否仍然只经过一个动态边界，而非多层套壳？
- TaskMonitor 是否可以在无 Pane/ImGui/ViewHost 的消费者中使用？
- workbench/widgets 与 viewport 是否不依赖 Scene/Material/Flow 作者模型？
- 暂时 BUSY、STALE、晚到完成、外部文件修改、代码最后 owner 等原回归是否保持？
- 是否真正删除旧来源路径、纯转发类型、旧 target alias 和重复 authority？
- 根 README 是否清楚区分目标架构、已经落地的新链和 P12 前仍存的旧产品入口？

新平台未测不伪造通过；旧收据保留；本次设计不自动放行 P11。

## 21. 来源与语言规则索引

### 仓库与交付事实

- **S1**：P10Q 实施与验收报告，参考当前实现 `3c20910d...`；包括目录/任务/发布/视口责任和验证限制。
- **S2**：`engine/process/README.md` @ `f7c27f9375cbf8dd8af37b30a6027a460de26213`。
- **S3**：`engine/scene/README.md` @ 同一提交。
- **S4**：`modules/core/object/README.md` @ 同一提交（现有上下文已读取）。
- **S5**：`editor/editing/sessions/include/lux/engine/editor/sessions/IEditSession.hpp` @ 同一提交。
- **S6**：`editor/persistence/include/lux/engine/editor/persistence/SaveSource.hpp` @ 同一提交。
- **S7**：`editor/persistence/include/lux/engine/editor/persistence/EncodeJob.hpp` @ 同一提交。
- **S8**：P02、P04、P06 R1、P07、P10 的冻结报告；仅用于继承明确的不变量，不据旧报告推断新行为。

### 标准与构建资料

- **L1**：C++ Core Guidelines，T.20/T.21（概念表达语义要求）、T.120 系列与接口/资源管理原则；本设计使用 C++20 子集，不因资料更新升级产品语言版本。
- **L2**：C++ 标准工作草案 `[temp.constr]`：约束和满足性规则；模板检查不自动提供运行时身份/寿命/原子性保证。
- **L3**：CMake 官方 `cmake-buildsystem(7)`：STATIC、SHARED、INTERFACE、usage requirements 与传递链接。

可复核公开来源：

```text
https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines
https://eel.is/c++draft/temp.constr
https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html
```

以上资料支持语言/构建机制；五层职责、命名、迁移顺序和删减判据是本次设计判断，不是源报告已经给出的实现。
