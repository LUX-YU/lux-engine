# Lux Engine：既有基建复用、通信方式与抽象颗粒度审阅

日期：2026-09-30  
仓库：LUX-YU/lux-engine  
分支：codex/editor-redesign-v4  
本次读取的 HEAD：`b9b856477755a9247a7a6810fa880ae62151f565`  
P10 实现：`11de2c1fee9d477daaa2d26b07911306ea9d6b9f`

## 0. 结论和证据范围

**关键底层设施没有被整体绕开，但上层设计没有因此自动成为一个统一、简洁、语义准确的 C++ 工程。** 当前源码可以确认：

1. 实际控件使用 LuxObject 的 TSignal/connect/Connection；但新业务连接并非全部沿用这套对象通信风格，存在裸 owner 指针＋函数表、只提供查询的 Port，以及每帧查询版本的路径。
2. 已核查的保存、Material 编译、Flow 编译/链接和桌面呈现路径复用 engine/process；没有证据表明这些路径另建了线程池或异步总线。领域操作记录与 Process 的 Task 不同，不能把所有业务记录都删成 TaskId。
3. GraphCanvas 使用 ax::NodeEditor，不是替换 imgui-node-editor 的另一个图形交互引擎。bootstrap 明确列出 LUX-YU/imgui-node-editor，widgets CMake 链接 imgui::node_editor。未核验本机已安装二进制的 Git 来源。
4. editor/adapters 当前只含 project_io；其中是正式发布执行及文件后端，不是调用旧 Editor 的兼容壳。目录按设计模式命名不理想，责任应回到相应功能模块；不能简单删除必要的文件发布和执行代码。
5. 确有可收敛的抽象：手工函数表式业务连接、无状态 Operation 类、单别名头、两层工厂、重复的图编辑交付状态机、重公共 include。它们需要逐项修正，不是把所有短文件或所有窄接口判为错误。

本次是定向源码审阅，不是全仓 AST/文件长度统计，不提供“多少百分比代码过度设计”的假精度。没有构建或修改引擎仓库，没有重跑 CTest/SDK/GPU，也没有执行完整归档核验。通过 GitHub 连接读到的源码、构建声明和此前冻结证据与设计判断分别注明。当前远端仍为上述 P10 HEAD，尚无可据此宣布 P10 R1 已完成的提交。

## 1. 必须重新明确的设计标准

用户的目标不是“层之间永远不知道对方是什么”，而是“一个完整工程内，责任准确、接口自然衔接、无需重复翻译相同事实”。因此：

- 同一业务事实只定义一份。跨模块使用该正式定义，不为证明禁边而重造同义类型。
- 允许真实的值转换，例如作者快照编码为文件、领域图投影为 UI；不允许两套同义协议长期通过桥双向同步。
- 目录按功能责任组织，而不是按 Adapter/Port/Manager 等模式组织。
- 只在真实多实现、插件边界、依赖方向或同步访问权限需要时引入接口。不将“每类先配一个 I 接口”当规则。
- 继承用于真实可替代角色，例如 Pane、LuxObject 的活动业务对象、确实存在多后端的存储契约；组合用于服务依赖和 owner；纯值不因通信需求被强制变成 LuxObject。
- 无状态的一次调用通常应是自由函数；只有确实有身份、在途状态、取消/结果责任时，Operation 才是准确的对象语义。
- 短文件不是缺陷指标；需要跨很多文件才能解释一个简单动作、却没有独立责任，才是分散。

**对前面指导的修正：** 阶段门禁擅长证明依赖未反向、结果未丢失、owner 唯一，但不能自动证明接口风格统一、公开头最小、调用链直观。此前将“窄接口”和“目录隔离”强调得过强，确实给手工函数表和微型包装留下了空间。原 V4 的类型/路径清单是目标草图，不应压过当前更明确的产品设计要求。

## 2. LuxObject：用了什么，哪些没有自然衔接

### 2.1 已经复用的部分

`modules/core/object/README.md` 明确规定：普通活动业务对象通过 LuxObject 获得线程归属、非拥有父链、事件和模板信号；Connection 是 move-only RAII 凭据。信号回调返回 void、必须 noexcept；带显式接收者的连接具备接收端寿命处理，无接收者 lambda 仅 DIRECT，捕获本身不自动延长寿命。[S02]

当前 GraphCanvas 使用 TSignal<CanvasEdit> 和选择信号；MaterialView/FlowView 用 LuxObject::connect 连接 edited/selected；SceneView 的工具按钮、点击与导航，以及 ViewHost 的 closeRequested 都走同一设施。[S03–S06]

所以不能下结论说新 UI 放弃了 LuxObject，或另造了一套完整信号系统。

### 2.2 确认存在的混合风格

当前 ProjectCatalogAccess 是：

```cpp
const void* owner;
Result<Version> (*version)(const void*);
Result<Catalog> (*read)(const void*);
Result<AssetId> (*resolve)(const void*, AssetReference, uint32_t);
```

AssetOpenRequests 又提供 void* owner + open；MaterialViewRequests 使用 void* owner + request(key, action)；TaskQueryPort 另有 owner + revision 函数指针。[S07–S09]

它们不是信号连接，没有 Connection 的接收者跟踪和取消语义；类型系统不能证明 owner 实际类型与函数表一致。注释要求 owner 活得比视图久，符合约定的调用可以安全，但安全性依赖手工装配。这不是已经证明的 UAF，而是正式业务接口的可维护性和契约表达不足。

函数表在 ABI、底层调度器、类型擦除端点中有合理用途。问题是同一个自有工程的具体业务对象，也被拆成手工 vtable，是否真的有必要；目前未见每一处都存在独立 ABI 或性能理由。

### 2.3 一个具体断开点：目录变化通知

现有 ProjectStorage 本身继承 LuxObject，并公开 catalogChanged 和 assetContentChanged，同时已经有 catalogRevision/catalog/resolveReference。[S10]

新 ProjectCatalogAccess 仅提供 version/read/resolve；旧 ProjectCatalogAdapter 再把 ProjectStorage 转成该函数表，复制目录值并翻译错误。[S07、S11] AssetPicker 每次 update 调 refresh，再查询版本。[S12]

这不能被称为“完全没有复用旧能力”，因为数据仍来自原对象；但通信和契约没有自然承接：提供者已有信号，消费者只看见手工查询表。

**收敛方向：** 正式项目模块拥有唯一目录事实和其窄公开访问，活动变更通知使用现有 LuxObject 信号；UI 借用正式的类型化访问，不永久依赖旧上下文转换器。可以将目录读取能力从过大的 ProjectStorage 中抽出到已有项目主题的实际 owner，但不能复制一份目录缓存作为另一权威。一次信号只提示需要刷新，读取和执行仍复核真实版本，不把信号当完整状态证明。

### 2.4 不是把所有调用都改成信号

| 行为 | 建议采用的既有机制 | 原因 |
|---|---|---|
| 控件事件、已提交事实、订阅变化 | LuxObject::TSignal + Connection | 与项目对象通信一致，解除订阅责任明确 |
| 需要立即返回 Result 的查询/准入 | 具体类型的方法或真正必要的窄接口 | 信号广播返回 void，不是请求/响应替代品 |
| CPU/IO/编译有限任务 | Process sender/receiver、Task/TaskScope | 保持统一执行、取消和结果交付 |
| UI 结构修改 | 原有安全点及有界请求 | 信号回调不能立即删除自己 |
| 数据值/快照/引用身份 | 普通 C++ 值 | 不需要为了通知而继承对象基类 |

特别不能把已接受任务唯一的完成值改走可能 FULL 的普通广播队列。完成事实仍由 Process 和业务 owner 可靠接收；需要通知 UI 时，在安全采用之后发布事实。

## 3. engine/process：关键执行链实际在复用

### 3.1 保存

SaveExecution 持有 process::TaskScope。编码调 tasks_.submit，在 execution.cpu() 上 schedule；发布调同一 TaskScope，在 execution.blocking() 上执行。完成是 TTaskResult 的 owner 回调，编码结果交给 SaveService，文件结果交给 WriteCoordinator。[S13]

这不是另一套线程池。P05 R2 的修正处理的是既有 Process 结果到业务服务之间的接收，不是新建调度器。

### 3.2 Material 和 Flow

MaterialCompileOperation::startSource 调 ExecutionRuntime::submit，在 cpu scheduler 上编译；TaskReporter 提供停止令牌和阶段。[S14]

Flow 编译链使用 CPU 生成对象，blocking scheduler 调 linker，再 continues_on(cpu) 编码；使用 process::Task 保存取消与寿命，不启动私有循环线程。[S15]

### 3.3 呈现与收尾

Desktop Presentation 使用 process::CompletionWork 和原 Runtime 唤醒/退休路径。[S16] Process 自身的文档明确：有限工作归 Process，场景无限运行/暂停/销毁不能整体变成某个 Process worker；工作完成、业务采用和 GPU 完成是不同事实。[S17]

因此 SaveId/WriteTicket/编译 key/RunId 不是仅凭“Task 已有 ID”就可删除的重复物。它们表达文件发布、固定来源、历史采用或运行结果；真正应删除的是只重新命名 Task::requestStop、却不承担新业务事实的空壳。

### 3.4 仍有可改善之处

TaskQueryPort::query/cancel 直接转发 Runtime，revision 是另注入的裸函数。TaskView 没有 revision 时，每次 update 都执行 taskInfos()，取得一份任务目录。[S09、S18] Runtime 已有 setTaskObserver。[S19]

这是源码可见的重复查询，不是本次测出的性能热点。优先让既有 observer 在正确 owner 上形成一次变更通知/修订，并由 UI 读取实际目录；不能每个 TaskView 都抢占 Runtime 的单 observer，也不新增第二任务管理器或第二目录权威。

## 4. GraphCanvas 没有替换 imgui-node-editor

有三层独立证据：

- bootstrap/lux-manifest.json 的 upstream repo 明确为 LUX-YU/imgui-node-editor。[S20]
- editor/widgets/CMakeLists.txt 查找 node_editor，并链接 imgui::node_editor。[S21]
- NodeCanvas.cpp 实际调用 ax::NodeEditor::CreateEditor/DestroyEditor/SetCurrentEditor，GraphCanvas.cpp 调 BeginNode/BeginPin/Link/QueryNewLink/QueryDeletedNode/GetNodePosition。[S22、S23]

GraphCanvas 的作用是将同一个 node-editor 后端接入 Lux UI，并将节点连接、删除、位置和选择转成应用可理解的 UI 意图。NodeCanvas 管理后端 context；NodeCanvasIds 处理 UI 身份；Material/Flow 的作者图仍在模型中。

这类集成本身必要：imgui-node-editor 处理画布和交互，并不认识 Lux 的 History、ContentStamp、Flow 函数或材质编译。不能为了删除所有封装，让每个工具重新写一份 NodeEditor 使用代码。

但封装不应扩张为另一个图领域模型、另一个撤销栈或一个“未来可换任意 backend”的万能图框架。当前更值得整改的是两个具体 View 对 BEGIN/PREVIEW/COMMIT/CANCEL/COMPLETE 交付流程的重复实现，以及 display/draft/queued input 三种来源没有统一纪律——P10 R1 已经直接发现后果。[S04、S05、S24]

bootstrap 的 revision 留空，所以本次证明的是源码及构建意图；没有核对本地 node_editor_DIR 实际安装版本。不要把它说成已验证了你机器上的库二进制。

## 5. adapters：目录命名不准确，但里面不是无用兼容代码

### 5.1 当前真实内容

当前 editor/adapters 只含 project_io。该 target 的两个生产 CPP 是：

| 文件 | 实际职责 |
|---|---|
| SaveExecution.cpp | 把保存编码和文件发布交给原 Process，完成后准确交回服务 |
| ProjectArtifactStore.cpp | 目标规范化、内容版本、临时文件、写入/替换/删除、发布结果与 reconciliation |

其 CMake 链接 persistence、process_execution 和 file_publication，没有旧 EditorContext/三工具 UI 依赖。[S13、S25、S26]

所以“adapters 中都是为了保住旧架构的壳”不符合当前源码。这里至少有正式文件副作用和明确执行责任。

### 5.2 用户对目录设计的质疑仍然成立

把正式能力放入 Adapter 分类，会让人首先看到“它桥接谁”，而不是“它负责什么”。SaveExecution 与文件发布后端还被归进同一个 project_io 包，虽然一个是保存调度接线，另一个是文件结果实现。

**建议的归属修正：**

- SaveExecution 回到 editor/persistence 的保存功能主题。若要保留纯协调核心不依赖 Process，可在同主题下维持必要目标边界，不需要 adapters 父目录。
- ProjectArtifactStore 回到实际文件发布/存储模块。当前实现只要求根目录和文件协议，未使用 ProjectStorage 的目录/资产模型；名称是否改为 FileArtifactStore，应按最终公开存储语义决定。
- 原 FilePublication 的底层算法保持唯一，不复制到另一个后端。
- 完成调用、CMake、安装和审计迁移后删除空 adapters 层和过期包入口，不以改名后继续保留两份实现作为完成。

这是功能归属和接口收敛，不是简单批量改文件名。也不意味着删除存储多态契约；真实后端、失败注入与文件事实之间可以有必要边界。

### 5.3 哪些才是需要清零的兼容壳

editor/transition/ProjectCatalogAdapter、LegacyComponentEditors、LegacySceneConfiguration、旧编译读取桥等，才是保住旧产品消费者的过渡接线。现有 P10 报告明确限定到 P12。[S27]

最终产品中不应再通过它们连接两套同义业务事实。用户拥有整个项目，能够同时修改提供者和消费者，因而不必永久保留旧源码接口或旧 C++ alias。

但“代码兼容壳清零”不意味着删除用户旧文件的读取支持。旧资产/布局解码属于对持久数据的明确支持，和长期保住 EditorContext 完全不同。

## 6. 已确认的抽象与颗粒度问题

### F01：手工函数表侵入普通业务连接

ProjectCatalogAccess、AssetOpenRequests、MaterialViewRequests 等将固定进程内对象擦除为 void* 和函数指针。[S07、S08]

问题不是每次会出错，而是生命周期、owner 配对、请求语义和错误映射都转为手工约定。MaterialViewServices 这种列明依赖的组合本身可以保留；应收敛其中过度擦除的具体请求端点，不恢复万能 Context。

优先让实际提供者给出统一类型化契约，事实通知使用 LuxObject。只有确有 ABI/类型擦除需要的位置继续使用低层函数表，并写清为什么它不适合正常类型接口。

另一个语义问题是 MaterialViewRequests 用同一个 `request(..., EMaterialViewAction)` 和 `MaterialCompileResult<void>` 表达 COMPILE 与 PUBLISH。[S08] 编译准入与文件发布准入原本是不同事实，P10 测试装配因此把发布错误包入编译/预览错误。应优先用既有编译结果和持久化准入结果定义两项准确调用，不为了共用一个 action 回调再增加万能错误袋。这是接口语义问题，不是声称当前真实发布算法未工作。

### F02：错误在桥接处被压成一种状态

makeMaterialView 的关闭回调将任何 cancelEdit 失败都返回 EViewError::BUSY。[S28]

这不是证明现在每种错误都能在合法路径触发，也不是新增崩溃复现；但转换实际没有区分“稍后可重试”与永久/身份/领域失败。接口窄到必须丢掉信息，是边界设计需要重新核对的信号。

修正目标是关闭准备的正式结果能保留可处理的分类和诊断，而不是给 Host 塞入所有工具的完整错误 variant。不存在信息损失的内部转发则无需包装。

### F03：没有操作状态的 Operation 类

PublishCompiledMaterialOperation 只有一个静态 start，没有实例状态；实际返回 WriteTicket，后续由 WriteCoordinator 拥有。[S29、S30]

推荐把它表达为材质命名空间中的发布准入自由函数。保留参数验证、来源不继承、预留失败回滚的真实算法；删除人为 class 壳。Flow 对称入口按其实际定义同样核对，不能仅因为名字相似就未经读取批量修改。

不要反向把真正有 task/key/completed/cancel 寿命的 MaterialCompileOperation 删成自由函数：两种类型本来就承担不同责任。

### F04：只有别名的独立公开头

SceneSessionAccess.hpp、MaterialSessionAccess.hpp 各只有一个 TSessionAccess<具体类型> 的 using。[S31、S32]

它们没有新增访问权限或验证，建议直接使用统一模板，或将确有领域可读性价值的别名放回恰当的既有公共头。不要保留“别名—转发头—新别名”的三层路径。

但将别名放进 Session.hpp 也需要检查是否因此新增不必要的 SessionStore 重依赖；不能为了少一个文件反而扩大公开头。直接在使用处写既有模板通常最简单。

### F05：两层工厂和微型转发文件

MaterialView::create 已构造和验证真实视图；单独 ViewFactory.cpp 再包成 DetachedView 并附关闭回调，声明却在同一个 MaterialView.hpp。[S08、S28]

泛型 Host 的 DetachedView 与具体视图 unique_ptr 可能是两个真实消费者层次，不能简单删掉其一。但是包装函数未必需要独立编译文件；可与本视图创建实现放在一起。是否保留两个公开创建入口，以实际调用者为准，而不是为扩展图表每个角色安排一个类/文件。

### F06：文件不少，核心交付逻辑仍堆在 View::Impl

MaterialView 和 FlowView 各自包含画布显示、属性缓存、待办队列、相同阶段状态机、命令开关、错误分类和编译展示。通用画布已经抽出，但相同交付规则仍复制在具体 View 中。[S04、S05]

这是“边缘拆碎、主流程仍集中”的例子。先明确显示快照、草稿及排队输入的正式关系，完成 P10 R1 的来源保持，再整理共同交付规则。若真正共享的只有少量算法，在已有 editing/UI 主题中收敛即可；不要为两份相似代码直接造通用 WorkflowManager/GraphController/通用大模板。

### F07：公开头的依赖没有充分收窄

MaterialView.hpp 的类实现已经是 PImpl，却仍 include SceneElement.hpp、MaterialPreviewStore.hpp 等实现层较重的头。[S08]

只以引用/指针出现的依赖可在合适的命名空间前置声明；SceneElement 是私有实现成员，应优先移到 CPP。值成员和错误 variant 需要完整定义的部分仍保留必要头。目标是完整公开契约，不是为了少 include 再制造一堆空壳 API 头。

## 7. 短文件应该怎样判断

| 样本 | 判断 | 处理方向 |
|---|---|---|
| SceneSessionAccess.hpp / MaterialSessionAccess.hpp | 纯别名，独立文件价值低 | 消除冗余路径或合入已有语义头 |
| PublishCompiledMaterial.hpp 中的静态 Operation | class 语义虚增 | 自由函数，保留算法和结果契约 |
| 单一工具 ViewFactory.cpp | 有转换责任，但未必有独立文件边界 | 和具体创建路径归组；保留真正需要的泛型工厂函数 |
| NodeCanvas.cpp | 很短，但管理 Create/Destroy 和当前 context 恢复 | 保留 RAII 能力，不将其叫作重复图引擎 |
| ContentStamp/代际身份/CodeLease/DetachedView | 小，但表达关键不变量或销毁顺序 | 保留；文件是否归组另按依赖决定 |
| 独立测试入口/生成输入/CMake | 短不代表生产逻辑碎片 | 与生产封装分开计量 |

P10 报告的“新增 93、删除 28”包含测试、CMake、生成器、从旧位置迁出的已有算法，并非 93 个新抽象。[S33] 不能把这个数字直接用作过度设计比例。另一方面，新增模块都是 STATIC 也不能免除认知、构建、公开头和安装维护成本。

## 8. 对既有阶段放行结论的校正

此前放行主要说明某阶段的明确职责和已列行为有实现/证据，并不等于已经证明全项目结构最优。项目现在有实质正确的核心，也有由阶段拆分和禁边驱动出来的可疑包装；两者可以同时成立。

不撤销已验证的：唯一历史算法/具体历史实例、会话 owner、冻结输入、文件发布/基线采用分离、运行与作者分离、真实视口隔离、底层 UI 安全点。

需要调整的：以“接口越窄越好、类型越多越明确”为默认目标；为了让依赖图通过就在中间造一张函数表；把无状态调用标成 Operation；把所有转换归入 adapters；为每个小角色建立独立文件/包；以 P12 删除期限为所有新增转换辩护。

用户这次明确要求整体结构重新审视，允许改变前文限定的目录和契约。那些“本轮不搬目录”的约束是阶段范围控制，不是永久架构正确性的证明。

## 9. 建议的有界收敛顺序

1. **先冻结通信与执行规则。** 事件/变化通知用 LuxObject；同步准入/查询用准确类型接口；有限异步用 Process；已接受完成可靠结清。不要新建通知总线或任务系统。
2. **完成已指出的 P10 R1。** 把草稿/排队输入与 based_on 绑定，真实 UI 测试穿过问题入口。不要靠本次结构整理掩盖原错误。
3. **整理永久接口的提供者归属。** 将 project/任务/编译请求的事实和窄访问放在真正提供者，优先直接组成，而非永久保留 UI 自建 vtable。
4. **取消 adapters 作为正式分层分类。** 将执行和文件发布归回原功能主题，同时修改消费者/目标/安装；不复制、不留旧 alias 壳。
5. **删无意义类型，归组短文件，减轻公开头。** 按真实调用者选择，不设文件行数阈值，不重新发明框架。
6. **重写剩余阶段的实施约束。** P11 不再把每个角色都展开成一层 Port/Registration/Adapter；P12 删除旧产品架构，但保留真实数据读取支持；P13 检查最终可读性、依赖和性能，不拿空壳规避删除。

这些是结构收敛方向，不是已授权修改仓库的命令，也不自动放行 P11。实施前应把每个调整列为“原事实 owner—统一类型—调用方式—删除项—必要回归”，避免把本次审阅再变成第三套架构。

## 10. 修正完成的判据

- 从控件事件到一次业务提交，可以沿现有信号/准确调用看清完整路径，不再跨数个只改名的包装。
- 无状态函数不伪装成异步 owner；真实业务操作与 Task 的差异有明确事实支撑。
- 新业务通信不为躲开依赖凭空使用 void*；确有低层端点/ABI 需要者保留并说明。
- 所有已接入的有限异步工作继续使用原 Process；无新增私有线程池、polling thread、executor 或 completion bus。
- NodeEditor 后端保持原选择，Lux 层只保留确有必要的对象/领域接线和历史来源约束。
- adapters 父目录不能成为长期责任分类；必要后端与执行能力仍有唯一 owner。
- 减文件必须连同重复类型/协议一起减，不是把同样复杂性挪进 Common.hpp；单个 View 也不能继续无限堆功能。
- 原真实 IO/GPU/SDK、操作所有权负例与已知失败证据保留；未运行项目不冒充已通过。

## 11. 固定源码索引

下列路径统一固定在审阅 HEAD；没有用公开搜索中的其他分支 README 覆盖本分支事实。URL 基址为：

`https://github.com/LUX-YU/lux-engine/blob/b9b856477755a9247a7a6810fa880ae62151f565/`

| 编号 | 相对路径 |
|---|---|
| S01 | 分支 ref：codex/editor-redesign-v4（读取值已固定于文首） |
| S02 | modules/core/object/README.md |
| S03 | editor/widgets/include/lux/engine/editor/widgets/GraphCanvas.hpp |
| S04 | editor/tools/material/ui/src/MaterialView.cpp |
| S05 | editor/tools/flowforge/ui/src/FlowView.cpp |
| S06 | editor/desktop/src/ViewHost.cpp |
| S07 | editor/project/ui/include/lux/engine/editor/project/ProjectCatalogAccess.hpp |
| S08 | editor/tools/material/ui/include/lux/engine/editor/material/MaterialView.hpp |
| S09 | editor/tasks/ui/include/lux/engine/editor/tasks/TaskQueryPort.hpp |
| S10 | editor/storage/include/lux/engine/editor/storage/ProjectStorage.hpp |
| S11 | editor/transition/ProjectCatalogAdapter.cpp |
| S12 | editor/project/ui/src/AssetPickerElement.cpp |
| S13 | editor/adapters/project_io/src/SaveExecution.cpp |
| S14 | editor/tools/material/preview/src/MaterialCompilation.cpp |
| S15 | editor/tools/flowforge/compilation/src/FlowCompilationService.cpp |
| S16 | editor/desktop/src/Presentation.cpp |
| S17 | engine/process/README.md |
| S18 | editor/tasks/ui/src/TaskView.cpp |
| S19 | engine/process/execution/include/lux/engine/process/ExecutionRuntime.hpp |
| S20 | bootstrap/lux-manifest.json |
| S21 | editor/widgets/CMakeLists.txt |
| S22 | editor/widgets/src/NodeCanvas.cpp |
| S23 | editor/widgets/src/GraphCanvas.cpp |
| S24 | 本对话 P10 R1 固定审阅文档：UI 草稿与排队操作的内容来源 |
| S25 | editor/adapters/project_io/src/ProjectArtifactStore.cpp |
| S26 | editor/adapters/project_io/CMakeLists.txt |
| S27 | dev_log/P10/README.md（以及用户上传的对应验收说明） |
| S28 | editor/tools/material/ui/src/ViewFactory.cpp |
| S29 | editor/tools/material/preview/include/lux/engine/editor/material/PublishCompiledMaterial.hpp |
| S30 | editor/tools/material/preview/src/PublishCompiledMaterial.cpp |
| S31 | editor/tools/scene/model/include/lux/engine/editor/scene/SceneSessionAccess.hpp |
| S32 | editor/tools/material/model/include/lux/engine/editor/material/MaterialSessionAccess.hpp |
| S33 | dev_log/P10/FILES.md（以及用户上传的对应文件清单） |

另外读取了 LUX-YU/imgui-node-editor 仓库元信息与上游 node-editor 公开说明，用于确认第三方职责；没有据公开搜索内容推断本机安装二进制身份。
