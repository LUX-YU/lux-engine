# P09 R1 复审结论与 P10 启动指令

**日期：2026-09-30**  
**仓库：LUX-YU/lux-engine · 分支：`codex/editor-redesign-v4`**  
**已审阅验收提交：`3363f83dcc448db9e79cea97c5176a546c05cf5a`**  
**对应实现提交：`c5bf69a2bb722741391984d5b584d3f90651eaeb`**

> **决定：P09 R1 本轮复审通过，允许进入 P10，仅执行 P10。**
>
> 本文件是原 V4 `P10_views_and_desktop.md` 的启动补充，不是替代架构。原七项 X10、相关 Q 和后续阶段责任不变。下文涉及新类型和方法的是实施目标，不冒充当前仓库已有实现。不得只按聊天摘要实施。
>
> 本次审阅是实际源码、提交差异、测试源码和部分归档记录复审；没有独立执行引擎构建、CTest、SDK、GPU 或完整归档验证器。没有新增独立引擎运行成绩。文件生成与引用检查不是产品测试。

## 1. 前置核验及证据边界

远端实施分支指向验收提交，其直接父提交为上述实现。已读取本轮 README、实现差异、当前迁移器、六组真实测试、修复前命令索引、部分前后 SDK 日志、完整 CTest 的结尾和归档检查器。[S01–S10]

### 1.1 已解决的具体问题

`prepareLegacyMigration()` 现在先解析 settings，取得 `selected_file`，再遍历所有旧布局；只有匹配选中文件的已知 locator 进入恢复清单。目录级 `locators` 合并表和跨文件 `legacy recovery binding` 冲突分支已撤销。每个文件内的 PaneId 重复仍明确拒绝，原几何和完整布局验证仍执行。[S03]

所有合法旧布局仍取得原稳定 LayoutId，完整原 TOML 继续进入各自 opaque envelope。非选 locator 没有放回新布局的活动视图状态。虽然 settings 的读取提前，`source_digest` 的规范拼接仍为排序后的布局路径/摘要，最后 settings 摘要；`continueMigration()` 原发布、来源碰撞和 marker 协议未改变。[S03、S04、S10]

空 selected、settings 确认缺失和所选文件缺失产生空恢复及诊断，不选择第一个文件。BUSY、IO、版本变化和坏输入仍拒绝；不能将它们解释成空目录或默认恢复授权。[S03、S04]

### 1.2 本次实际读到的前后证据

| 场景 | 修复前记录 | 修复后记录 |
|---|---|---|
| Alpha/Beta 同 PaneId/type、不同资产，选 Alpha | 两个文件单独合法；合并被 `legacy recovery binding` 拒绝 | 两布局均保留，恢复项为一项 Alpha，随后实际发布并重读 |
| 不同 key 的非选 Beta | prepare 成功，但 `recovery_entries=2 selected_scope=0` | `recovery_entries=1 selected_scope=1`，只含 Alpha |
| 中断后重建 Store | 新增测试在原代码上首先遇到同一目录冲突 | 新代码测试销毁实际 Store、保留用户 label 修改、完成剩余四项发布并验证选中恢复 |

前两行前后日志的输入摘要相同；这证明改变的是来源规则而非换了一组输入。第三行的执行成绩来自提交报告，具体断言已读取；不是本审阅环境复跑。[S04–S08]

归档记录 Editor **180/180**、PLAYER **11/11**、13 组 SDK **93/93**，含原八项 operation 编译负例；两个显式 GPU 模式单独记录。已阅读检查器对原 174 项、六项新增测试集合、原测试体、保护路径、来源摘要和前后日志的检查，但没有执行该检查器。[S02、S09、S10]

### 1.3 不得扩大为已经完成的事情

- 没有证明任意旧格式、任意外部并发写者或所有异常组合均无缺陷。
- R1 **不自动重写**已经存在的同源 recovery 或完成 marker；它们可能包含用户修改。旧版本曾产生的目录并集不因本次补丁自动回滚。确需清理既存用户数据时另有明确授权和保留策略，不能在 P10 偷改用户文件。[S02、S04]
- 未宣称资产已经自动恢复、旧 C01 已修复或新桌面已经完成。
- 干净源码检出复用原构建树，不等于首次冷构建资格。
- 旧两个目录名包含 GPU/ScenePane 的默认 CPU_UI 成绩不能算作 GPU。后续继续使用实际 `CONSUMER_MODE`、test command 和日志分类。

## 2. P10 的目标和不能提前承担的工作

**目标：让既有作者模型、交互、投影、运行和保存能力，经正式新 UI 模块在真实桌面中共同工作，证明窗口不是内容 owner。**[P10]

| 本阶段必须交付 | 明确不在本阶段完成 |
|---|---|
| 生产 ViewHost、DesktopShell 和三类具体视图 | P12 的唯一产品入口切换与所有旧框架清零 |
| 作者双视图、作者/运行并存、真实双视口 GPU 与验证层检查 | 用原旧 ScenePane GPU 回归替代新 SceneView 组合 |
| 新 Outliner/Inspector/资源/创建/任务/项目/资产选择的功能切片 | 为辅助工具伪造编辑会话和 History |
| P08 离树工厂、挂载、关闭、身份与重绑定接线 | P11 完整动态扩展、命令注册及插件发布体系 |
| P09 纯布局计划被宿主有限采用的技术能力 | P12 完整 ApplyLayout/RestoreSession/Open/Close 用例编排 |
| 不安装的真实集成 harness，正式新模块承载功能 | 安装第二个 Editor 产品、永久 Old/New 选择开关 |

P10 可以在 harness 装配根静态创建正式模型和服务、调用正式工厂；这是绕开尚未实施的动态注册，不是绕开真实模型、Runtime、IO 或 GPU。不得为演示再写一套“测试编辑器算法”。

原 P10 明确要求实际双视口 GPU/验证层测试。缺该环境只能报告 BLOCKED/PARTIAL，不能将整个必测项移到 P13。完整产品层的所有跨平台、像素、IME 资格仍按 P12/P13 范围继续，不反过来否定本阶段必须执行的实机切片。[P10 §C2、H]

## 3. 文档优先级、工作区与账本

1. 从上述已验收 HEAD 继续；实际 HEAD 若有后续改动，先核对祖先与差异，不 reset、不覆盖。
2. 保留 `ProjectBuilder.cpp` 用户修改；既有报告记录其 SHA256 为 `ccac49618140d014236c9f23b55fc15d31c96e94b44395dee39fd67d35ab1f5c`。该值是交接证据，不是授权直接覆盖当前文件。实际差异若变化，另行记录。
3. 唯一可变材料继续为 `.internal/editor-redesign/`；阶段末冻结 `dev_log/P10/`。原 V4 中 `docs/editor-redesign/receipts` 为历史路径，不能再建立第二份可变账本。
4. 原 V4 类型/文件表为目标语义和删除对应关系。先按现有迁移账本定位真实文件，已经迁走的定义不得重新创建。
5. 后续实施优先于历史草图的已确认调整包括：history/sessions 收到 `editor/editing/`；P01 私有 `currentContent()`；同一读取 gate；编译 operation 的唯一公共 owner；P09 ViewInfo 迁入 contracts；GPU 模式显式化。
6. 本阶段结束后停在 P10 等待复审，不自动开始 P11。

## 4. 固定的所有权与依赖图

```text
装配根 / 集成 harness（将来由 P12 产品装配接管）
  ├─ SessionStore ──独占── SceneSession / MaterialSession / FlowSession
  ├─ 原保存角色 / SaveService / 唯一 WriteCoordinator / 真实文件后端
  ├─ 原 RunStore ──独占── RunSession（Runtime 负责实际实例）
  ├─ 原投影 / 编译 / 预览 / 具体交互的拥有者
  └─ DesktopShell（平台、Root、UI 呈现）
       └─ ViewHost : IViewHost
            └─ 顶层 Pane 的唯一拥有单元
                 ├─ SceneView : Pane
                 ├─ MaterialView : Pane
                 ├─ FlowView : Pane
                 └─ 辅助 Pane / Element
                      └─ 各自明确拥有子控件

视图只持受限访问、身份、视图状态及本视口呈现责任。
Root 的路由/父链/注册不是第二个删除 owner。
Runtime 仍只有一个实际 frame 驱动 owner。
```

图中“装配根持组件”不是新 EditorContext。不得把装配根作为通用参数传给所有控件，也不得通过 `services.get<T>()` 绕开模块依赖。

ViewHost 只理解视图身份、Root 协议、布局值及拥有责任；它不能依据运行是否存在而替工具决定作者读写路由。DesktopShell 不拥有作者模型、SaveService、Source，不解析 Scene/Material/Flow 的具体绑定。[P10 §B–C]

## 5. 类型、关系、成员和目标文件

**下表的新名字来自原 P10 目标或其局部状态说明；不是要求每一行新增一个文件/target。已有等价类型优先复用。**

| 类型/角色 | 关系和唯一责任 | 允许成员/能力 | 明确禁止 | 归属 |
|---|---|---|---|---|
| ViewHost | 实现 `views::IViewHost`；拥有顶层 Pane 单元 | 有界槽、Host 域/generation、pending view 请求、准备/退休记录、只读 ViewInfo | Session/Run owner、内部容器外借、通用 Context | `editor/desktop/` |
| DesktopShell | 组合平台/Root/呈现；与 Host 组合或明确借用 | WindowInput/Output、Presentation/UiRenderSyncStage 的唯一平台算法 | 作者保存/加载逻辑、第二场景驱动 | `editor/desktop/` |
| SceneView | `final : ui::Pane`；组合绑定和视图状态 | SceneViewBinding、相机/尺寸/hover、ViewportPresentation、窄访问、交互组关联、UI 子控件 | 可写 SceneSource/Package、History、checkpoint、SaveOperation、RunSession、运行实例 lease | `editor/tools/scene/ui/` |
| SceneViewBinding | 封闭 variant，不用无效 key 伪装完整状态 | Unbound、EditedSceneBinding、RunningSceneBinding；携带准确来源域 | 依靠 `run_scene != nullptr` 隐式切换作者/运行 | 与 SceneView 同主题 |
| SceneViewState | 视图本地值 | 相机导航、输出需求、悬停/捕获等 UI 状态 | 复制作者相机组件作为第二源 | 同上 |
| OutlinerView / InspectorView / ResourceView / SceneCreationView | 真正 Pane 或 Element 继承；具体领域适配 | Scene/Run 窄访问、领域批次/请求、资源查询/重试 | 访问旧 SceneEditor::Impl、Session::Impl friend | `editor/tools/scene/ui/` |
| MaterialView | `Pane` + 明确零/一绑定 + 图交互/预览访问 | MaterialViewState、typed key、受限 MaterialViewServices、输入反馈 | 自动新建/销毁会话、持 `compilation_` 控制 owner、源/History 副本 | `editor/tools/material/ui/` |
| FlowView | `Pane` + typed key + FlowInteraction 关联 | 图/变量/函数/导出/字面值 UI、固定对象重试反馈 | 自己重编当前图代替 retryLink、拥有源或 compiler | `editor/tools/flowforge/ui/` |
| makeSceneView / makeMaterialView / makeFlowView | 普通工厂函数 | 受限 create info/services → 完整 DetachedView | 在工厂中 adopt/reuse/focus/open asset | 各工具 `ui/src/ViewFactory.cpp` |
| TaskView / TaskQueryPort | UI 与窄查询/取消请求 | 读取真实执行器任务事实 | 第二任务管理器、虚构完成百分比或终态 | `editor/tasks/ui/` |
| ProjectView / AssetPickerElement | 项目/资产 UI 与专用接口组合 | ProjectCatalogAccess、AssetOpenRequests；选择地址或发请求 | 返回任意 Pane 引用、持 EditorContext、伪造 History | `editor/project/ui/` |
| widgets | 通用纯 UI 算法 | 输入、列表、基本字段等无领域实现 | SceneRuntime、SceneEditor 或模型私有访问 | `editor/widgets/` |
| Inspector codegen 适配 | 工具层生成接口 | 生成领域字段意图；host codegen 明确构建依赖 | 输出裸 ECS 可写指针、把生成器公共链接进运行库 | `editor/tools/scene/ui/codegen/` 等原 P10 位置 |

当前 `IViewHost` 只公开 close/show/focus 请求及拥有型 describe，`DetachedView` 已是 move-only 拥有单元；直接实现它们，不另建同义接口。[S12]

**ViewInfo 唯一定义现在是 `editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp`，公开 include 仍为 `lux/engine/editor/views/ViewInfo.hpp`。** 不得在 `views/api/include` 重新放一个定义或转发头。[S13]

ViewId、ViewRestoreKey、ViewTypeId 和 PaneId 的域继续分开：活动操作用 Host+slot+generation；恢复匹配用稳定恢复键+类型；PaneId 只是底层登记身份。不得把 LayoutId/restore key 的 hash 当 live handle，也不得仅按 slot 或指针比对延迟请求。

### 5.1 特殊成员和资源清理

- 活动 Pane/固定地址 Host/控制 owner 不可复制；确需转移顶层拥有权时移动 owning 单元，不按值移动活动 Pane。
- `DetachedView` 与一次准备 token 沿用已验证的 move-only 和旧内容清理顺序。Host 槽容器增长不得复制或错误析构挂载单元。
- 不可为使容器编译而重新开放 MaterialCompileOperation/FlowCompileOperation 的复制/按值移动；Material 转移 `unique_ptr`，Flow 借 ID/const 操作引用。
- 子控件若是值成员，它已是唯一 owner；不可再包装为 owning `unique_ptr`。Root 不 delete 同一节点。
- 视图关闭首先移交视口退休责任，代码/资源仍被真实在途任务使用时继续按原寿命协议持有；不能把 UI 节点销毁当 GPU 已退休，也不能永远保活整图作为替代。

## 6. P10 内部实施顺序

以下 A–G 是同一阶段内的依赖顺序，不要求每步另行等待授权。完成局部步骤不是阶段 PASS。

### P10-A：真实调用方、成员和功能盘点

从当前 `editor/ui/CMakeLists.txt` 及各工具旧 UI 起点列出实际文件、导出、codegen 输入、测试入口。当前旧 `editor_ui` 仍同时编译平台输入/输出、场景字段/配置、TaskPane、AssetPicker 和呈现，并链接旧 Context、编辑和存储；不能整个改名为 common 供新视图继续依赖。[S11]

为每个旧成员标注：纯 UI 值、作者源、历史、保存、运行、编译、投影、输入、观察、代码保活。每项明确新 owner、迁出的唯一算法、暂留消费者和期限。新 SceneView 逐成员使用第 5 节白名单审核；其他两个工具同理。

建立功能矩阵，不只列“窗口能显示”：Scene 结构/字段/配置/创建/拾取、Material 图与参数/预览/发布、Flow 变量/签名/导出/字面值/重试、Task 状态/取消、项目和资产选择、资源失败/重试、Inspector 生成字段、项目/launcher 用户入口。矩阵每行对应真实操作和观察，不用新窗口截图替代。

### P10-B：先真实宿主与窄视图协议

先落实正式 `ViewHost` 和 `DesktopShell`，最小具体 Pane 可用于宿主测试。正式桌面能力在新模块中；测试仅负责配置故障和驱动，不复制正式宿主实现。

宿主基本流程：

```text
完整离树工厂结果
 → Host 准备槽、身份、请求与退休容量
 → Root 原准备协议
 → owner 安全点提交拥有权/注册
 → 精确发布视图事实
 → 提交后通知/诊断
```

准备失败：候选仍由明确的一方拥有并能销毁；原 Root、槽、焦点和 Session 不变。提交事实和通知结果分开；通知失败不能把已经挂载报告成没有发生。通知期间释放调用方 token 或递归发 close，不得导致正在执行的记录失效。

ViewRequests 使用完整 ViewId 排队；队列容量拒绝是明确结果。drain 固定当前批次，回调中新请求进入后批；在回调或 draw 栈上不立即删除当前节点。非法旧代际请求不能命中新挂载对象。

Root 挂载后通知与 Host 发布之间的重入可见性须写成一条明确协议：回调看到已提交状态或准确的不可重入状态，不能观察到可操作的半挂载 owner。不得为简化顺序将 Root 已有通知安全检查关闭。

show、focus、adopt 分开。modal 拒绝聚焦不是构造失败，不能销毁已成功创建的视图。活动状态查询返回拥有型 ViewInfo，不返回 owning vector/span<unique_ptr<Pane>>。

### P10-C：SceneView 与场景辅助视图

先接 Unbound 和作者绑定，再接运行绑定。Unbound 是完整合法空视图，禁止用非法 SessionKey 继续走作者读写。

作者 SceneView 通过 typed key、短时访问和具体 interaction 发领域批次；不跨帧保存可写 Registry/field pointer/EditScope。运行绑定按 RunId+实例身份查询，保持暂停/控制契约。不能因为 Entity 数值相同误走作者路径。相机导航是视口状态，不自动修改作者相机对象。

接入原 ScenePresentationHub/SceneProjection/ViewportPresentation：共享作者投影，不共享本地相机、尺寸、hover 和输入捕获。高亮必须走 P07 真正的 view-local 后端目标；不复制整场景逃避隔离。

Outliner/Inspector/Resource/Creation 控件调用受限接口。Inspector 按当前 schema 生成字段提交，资源重试与创建通过准确请求完成；不回到旧 SceneEditor::Impl friend 或裸 Registry 编辑。

### P10-D：绑定替换、关闭和实际双视口

重绑定先准备新观察、交互关联和呈现所需责任；失败保留旧绑定、相机和可用内容。不能先清空旧 view，再尝试新源。成功交换后再结清旧观察和视口资源。可等待的资源状态应报告 pending/失败，不能默认成功或依靠后台最终补齐错误的绑定。

关闭一个视图不关闭作者 Session；关闭最后一个视图也不能未经 P12 的内容关闭用例就销毁作者。运行视图关闭与 stop Run 同样是不同请求，不能让视图析构偷偷拥有 stop/释放整个实例的责任。

真实切片至少包含：两个作者 SceneView 共享一 Session、不同相机/尺寸；一处编辑和 Undo 使两处显示收敛；关闭一个并重新创建，Session/History 不变；再加入运行视图验证独立来源。每视口关闭保留其退休责任直至实际 GPU 完成。

本步骤运行真实 GPU 和验证层，在有明确输出判定的场景中检查双输出确实不同/随模型更新，而非只创建两份 ViewInfo。精确观测方式由当前后端可用能力确定并记录，不假称未取得的像素读回。

### P10-E：Material、Flow、任务与项目工具完整接线

MaterialView 保留原图、常量/槽配置、编译/发布入口与预览导航。FlowView 保留变量/函数/导出/字面值/连接和链接重试。编译提交/查询/取消由受限接口接往原服务和装配 owner；视图只持身份、观察状态和本地 UI，不持任务控制对象的独占业务责任。

P07 当前 Material 编译的具体控制对象由 `start()` 返回 unique_ptr；不要仅因原文使用“服务”一词就凭空实现另一个通用编译管理器。装配层可以用有限记录拥有既有 operation，再向视图提供所需窄入口；责任、容量和关闭时保留/取消策略明确，不把这些记录塞回 View::Impl。

Flow `retryLink` 固定原对象和原内容戳，不能在按钮回调里读取当前作者图重新编译。Material 新状态失败时旧效果可以保留，但必须保留旧 stamp 并明确 stale，不能显示成当前成功。

TaskQueryPort 反映真实任务事实；ProjectCatalogAccess 只读查询，AssetOpenRequests 仅表达应用请求，不返回 Pane 作为“打开资产成功”。本阶段 harness 可用静态、受控处理器把请求接到既有模型/保存/运行服务；完整应用用例仍按 P12 交付，不在视图中实现另一份加载和保存状态机。

### P10-F：提取旧 editor/ui 的唯一算法并完成新集成链

按第 8 节逐文件迁移平台、领域和通用部分。可以供新旧共享的纯算法移动到唯一位置，旧产品仅保留转换壳；不能复制一个新 namespace 后将原实现留着继续演化。

harness 只负责装配正式模块、静态调用工厂、制造输入/故障和验证。包含真实 SessionStore、三模型、SaveService/WriteCoordinator、RunStore、投影、编译/预览、ViewHost 与三工具视图。没有旧 Context/PaneManager/三大 Editor include 或链接绕路，不安装、不增加用户可见第二产品。

P09 的计划可以在技术切片中用于创建/复用视图壳，但不读取 opaque 中的历史 locator 发起恢复，不把全部布局目录当一次恢复会话。未来 P12 读取独立 RecoveryManifest 来决定内容恢复；本阶段静态装配的测试内容明确标记为测试输入。

### P10-G：失败矩阵、安装与阶段封存

执行 X10-01～07、相关 Q、现有模型/保存/运行/交互及历次 R1/R2 回归。确认新生产路径的真实 include/link 闭包，不允许整个旧 editor_ui 经 PUBLIC、PRIVATE 或静态传递边进入新 harness。

依赖迁移允许旧测试改用等价新 API，但必须给出对应表并保留等价或更强断言。不同于纯窄补丁，本轮本来就迁移旧 UI；不能为追求“逐字未改”而保留两份生产算法，也不能借迁移删去原功能/故障语义。

最后分别提交实现与 `dev_log/P10` 验收。必测缺环境/缺结果、到期新路径副本未删，只能 PARTIAL/BLOCKED。

## 7. 跨子系统失败语义：直接继承，不重新踩同类问题

| 已建立的边界 | P10 必须如何使用 | 禁止的退化 |
|---|---|---|
| P01 当前内容戳/保存基线/准入唯一 | 视图读准确值并发窄请求 | 增加 view.current、dirty、save_pending 作为另一份权威 |
| P02/P03/P04 有序批次 | 手势最终值经原领域一次提交；删除/重建后不继续使用旧字段地址 | 每帧修改作者源后再“合并历史”冒充独立 Preview |
| 同一读取 gate | UI 字段展示、codec 和扩展清理遵守原同步借用/准入契约 | 跨帧持 read/edit scope，或多枚 busy 协调同一门 |
| P04 身份高水位 | 节点/Pin/变量通过原算法创建与恢复 | 重绑定或另存为重建整图，令旧 ID 分给新对象 |
| P05 接收/发布/基线采用分离 | UI 分别显示请求、真实发布和采用事实 | 文件已经写入但会话关闭，就把磁盘成功抹掉 |
| P05 R1/R2 防重入与可靠完成 | 外层 UI/服务回调期间，已准入完成可靠记录，嵌套不解除外层保护 | 因 BUSY 丢结果、重跑 encoder、忽略返回值导致永久 pending |
| P06 实例退休/结果确认分离 | Run 单步结果查询到单项或整体确认；视图关闭不伪造确认 | 重实例删除后把未确认结果当 INVALID_ID 或擅自推算取消 |
| P07 版本/后端视口隔离 | content/settings/environment/target 共同匹配；高亮按完整视口 | 同场景全局高亮或迟到 S10 覆盖 S12 |
| P07 R1 公共操作 owner 唯一 | 外部结果可复制，控制对象通过指针/ID/借用 | `auto operation = borrowed.get()` 再造会取消原任务的 owner |
| P08 手势、离树、安全点 | 编码不含临时预览；关闭请求先排队 | 工厂构造就注册、draw/signal 中 delete 自己 |
| P08 R1 临时失败不等于消失 | Store BUSY 返回原错误，保留 selection/gesture/绑定；STALE 才失效清理 | 一次查询失败就把视图改成 Unbound 或取消用户输入 |
| P09/R1 布局/恢复/偏好/目录分离 | 纯布局只组织视图；仅明确恢复清单处理内容 | 按目录聚合资产、解析非选 opaque 发起 open/rebind |

这些不是要求每个子系统各造一套通用事务、状态表和错误框架。优先复用已经验证的明确接口；发现接口不能表达真实协作时，在所属模块做最小扩展、补测试和对应说明，不能在视图私有状态中复制权威事实。

### 7.1 重绑定与手势的特殊组合

原绑定正在 Preview 时，重绑定失败必须保留原绑定和原手势；成功重绑定需要明确结束旧交互，但不能为了准备新视图抢先提交作者修改。若旧交互正因 Store 回收而 BUSY，准确等待/拒绝，而不是伪造“旧对象已经消失”。

关闭一个共享选择组的视图，不等于删除共享组或关闭 Session。选择组的实际 owner 和最后使用者规则在 P10-A 表中写清。相机与输入 capture 不因共享选择而共享。

### 7.2 容量与请求事实

挂载、请求队列和退休使用声明的有界容量；容量拒绝不夺取调用方仍需保留的候选。不为每一小类状态增加新库。目标已卸载的过期排队请求可明确失效，不产生新操作；已被接受的异步资源/任务完成则须由原 owner 结清，不因 ViewId 失效丢弃资源责任。

## 8. 旧文件、函数与成员的迁出/暂留表

下表以原 P10 D/G 表为责任依据，实际路径由本阶段 A 核对。本次已读当前 `editor/ui/CMakeLists.txt`，所列平台/领域文件仍在旧 target 中。[S11]

| 原位置/符号 | 本阶段新路径动作 | 删除什么 | 暂留什么及期限 |
|---|---|---|---|
| `editor/ui/src/WindowInput.cpp`、`WindowOutput.cpp` | 平台输入/输出算法提到 desktop | 已迁算法原体、无用 include、旧无条件 Context 查询 | 旧产品需要的参数转换/入口壳，限定消费者，P12 |
| `editor/ui/src/Presentation.cpp`、`UiRenderSyncStage.cpp` | 同一呈现/同步算法由 desktop 使用 | 第二份 presenter/input loop；不复制整个实现 | 必需旧产品接线，P12 |
| `editor/ui/src/TaskPane.cpp` | TaskView + TaskQueryPort | 新路径 Context 取任务和重复状态计算 | 旧 Pane 名称/菜单接线，P12 |
| `editor/ui/src/SceneConfigurationElement.cpp` | Scene 工具 UI 的领域配置批次/请求 | 裸作者源写入和旧 SceneEditor friend 路径 | 旧产品转换，P12 |
| `editor/ui/src/SpatialInteraction3D.cpp` | 数学/手势归 scene interaction，事件适配归 scene UI | 两份拾取或逐帧作者写入逻辑 | 必需旧 Runtime 调试适配，逐项登记，P12 |
| `editor/ui/src/SceneElement.cpp` | 新 Scene UI 只保留绘制/输入，派生责任继续归 P07 | 重新产生的视口资源 owner、副本高亮/输出状态机 | 旧 UI 外壳/安装入口，P12 |
| `editor/ui/src/ComponentEditors.cpp` | 工具级 schema/字段适配 | 裸 ECS 指针跨帧、对 SceneEditor::Impl 权限 | 有限旧调用适配，P12 |
| `editor/ui/src/CodegenInput.cpp`、原 codegen/cmake | 迁到 scene/ui 的 host 代码生成集成 | 新 generated 中旧访问头；错误 PUBLIC 生成器依赖 | 旧调用的构建脚本适配，P12；算法只一份 |
| `editor/ui/src/asset/AssetPickerElement.cpp` | project/ui；基本列表/输入可归 widgets | 打开资产返回任意 Pane/Context 的新路径 | 原旧产品请求翻译，P12 |
| SceneContentElement/OutlinerPane/InspectorPane/ResourcePane 的 friend | 新控件全部使用窄能力 | 新路径 friend、TestAccess 后门、旧大类 include | 仍仅旧工具使用的 friend 随旧类 P12 删除 |
| Material/Flow 旧 Elements | 新具体视图完整复用领域/编译能力 | 源/History/编译状态 owner 副本、纯算法原体 | 未切换产品的 UI 转换，P12 |
| 旧 `editor_ui` 聚合 target | 新链不得链接；纯算法提到精确归属 | 把整个库改名为 common 的永久旁路 | 原产品继续使用到 P12，届时删 target/导出/安装入口 |
| P08 rooted ctor、旧 `root()` 假定 | 新工厂/新控件使用 detached 与显式 attached 查询 | 新消费者调用兼容入口 | 原登记旧产品/回归到 P12 |
| P09 `ViewInfo.hpp` 原 views/api 位置 | 保持 contracts 唯一定义及公开 include | 不重新创建原文件或同义 alias 层 | 无新增旧定义 |

对一个文件中同时存在可复用算法和旧产品转换的情况，不要求立即删除整个文件；必须删除迁出的原算法体，将保留部分缩成真实、有限的转换。若文件已无职责则删除整文件，不能留下空转发或改名 backup。

删除同步覆盖声明、定义、调用、friend、生成输入、CMake target/source、安装导出、示例和测试。需要更改历史测试绑定新 API 时，记录语义映射；冻结的旧收据与失败样本不改写。用户资产/旧布局备份/opaque 数据不属于待删代码。

## 9. 构建目录和依赖门禁

```text
editor/
  contracts/                   # ViewInfo 当前唯一定义等轻量值
  editing/history/
  editing/sessions/
  views/api/                   # IViewHost / ViewRequests / DetachedView
  desktop/                     # ViewHost + DesktopShell + 平台/呈现算法
  widgets/                     # 真正通用纯 UI
  tasks/ui/
  project/ui/
  tools/
    scene/ui/                  # 主视图、辅助视图、领域 Inspector/codegen
    material/ui/
    flowforge/ui/
```

交互、projection、preview、compilation 留在现有工具主题目录。原 P10 指定 `tasks_ui`、`project_ui` 是明确边界，不是新 editor_common。一个语义头可以容纳相关小值；不为每个 ID、binding 分支和票据增加 target/DLL。

至少增加并实际运行以下禁止边负例：

| 边界 | 禁止依赖/行为 |
|---|---|
| Engine UI | 反向依赖任何 editor 目标 |
| view_api / ViewInfo | 具体作者模型、ProjectStorage、RenderRuntime、CommandRegistry 实现 |
| ViewHost 控制部分 | 具体 SceneSession/MaterialSession/FlowSession/RunStore 实现或旧 PaneManager |
| 作者模型 | 反向链接新 UI/interaction/desktop |
| 通用 widgets | SceneRuntime、领域模型私有头和旧工具大类 |
| 新工具 UI | 旧 Context/三大 Editor/旧 editor_ui/transition 桥的直接或传递入口 |
| 新 harness | 上述旧业务目标经 PUBLIC/PRIVATE/imported/静态闭包绕回 |
| 新工厂 | rooted ctor/构造中 Root 注册 |
| 安装消费 | 源码目录的私有头或构建目录 DLL 补齐缺失安装依赖 |

这里“ViewHost 控制部分”不等于禁止 DesktopShell 的正式 GPU/UI 呈现组件依赖其必需渲染模块。若为其建立精确 target 边界，按真正依赖决定，不机械切成几十个库。每条负例须命中预期规则；修复同一夹具后通过。不能把缺依赖、语法错或缺 GPU 当依赖门禁成功。

## 10. 原七项 X10 的验收内容

保持原编号。下列展开用于防止把局部 stub 或旧产品运行当新切片，不另造 P10.1/P10.2 等平行阶段。[P10 §H]

| 原项 | 必须使用的真实对象/操作 | 至少记录的观察 | 不足以通过 |
|---|---|---|---|
| X10-01 双视图共享作者 | 两个正式 SceneView、同一真实 SceneSession/History、正式投影、真实 GPU | 两视图可实际编辑/Undo并收敛；相机/尺寸独立；单一 Runtime frame owner；两个输出可判定；验证层错误数 | 两个 FakeViewInfo、复制场景的两个旧 Pane、只显示空窗口 |
| X10-02 关闭单视图 | 双视图关闭一处，再创建另一处 | SessionId/HistoryId/内容/绑定不变；另一个输出可用；旧 ViewId 不能命中新 slot；资源退休与释放计数 | 关闭时偷偷保存/销毁作者，或永不释放资源 |
| X10-03 作者/运行隔离 | 同源启动 Run，作者与运行视图并存 | 原/新来源引用不误路由；作者和运行导航/编辑不混；stop 后准确失效；未确认 step 结果仍按原协议 | 只检查 UI 标签、仅比较 Entity 数字 |
| X10-04 完整功能矩阵 | Material/Flow/Task/Project/资产选择/创建/Inspector/资源重试的实际新控件 | 每项输入→请求/领域结果→UI反馈；编译/发布/重试用真实服务；作者状态和产物事实分开 | 仅迁移画布，遗漏变量/签名/辅助工具或按钮默认成功 |
| X10-05 工厂/挂载/重绑定失败 | 正式 Host/Root、完整候选与实际旧绑定 | 构造失败无注册；准备容量失败不夺取旧绑定；后通知失败保留提交事实；重绑定失败相机/交互仍可用；节点/code只释放一次 | 用 FakeHost 代替正式 Host；先 clear 再 try |
| X10-06 输入/capture/焦点/IME | 新桌面的拖拽、文本组合、焦点切换、draw/signal 中关闭 | Begin/Preview不入作者快照，Commit恰一次；close不在栈内delete；capture/焦点按阶段释放；实际IME与模拟事件范围分开 | 只调用模型API，无输入路径；合成事件冒充真实系统IME |
| X10-07 新链无旧依赖 | 新集成 harness 的真实编译/链接/安装闭包 | 无旧 Context/PaneManager/Editor；正式工厂/模块承载行为；harness不安装、无Old/New产品开关 | 静态检查只搜头名、借旧 editor_ui 链接所有实现 |

### 10.1 GPU、IME 和范围报告

P10 的新双视口实机 GPU/验证层检查是必测，不得只重跑旧 `GPU_UI` 与 `EDITOR_SCENE_PANE` 两项就声称满足 X10-01/03。旧两项仍作为回归并单独记录。记录实际 `CONSUMER_MODE`、可执行命令、测试视图类型、设备/验证层、输出证据和退休事件，不以目录名证明运行模式。

X10-06 需要通过新桌面的真实输入/capture/焦点生命周期。文本组合的系统 IME 实测与受控合成事件分开，明确未覆盖部分；不能把尚未实测写成 PASS。原规范允许实测范围和未测范围分开报告，不借此把整个输入条目关闭。P13 完整多环境认证仍独立。

若 GPU、桌面或其他必需条件不可用，交付已经完成的代码与证据，状态 PARTIAL/BLOCKED；不能为了通过修改图形后端、关闭验证层或永久 Unsupported。

### 10.2 容量与性能结论

复用原有有界 Host/服务/退休策略，记录多次打开/关闭/重绑定后的容量恢复。不要仅凭 32/64 次循环推断全部内存泄漏已排除，也不要把总进程耗时当预热热路径性能。P10 不引入额外的新性能研究任务，但原 Q50/Q51 所需的重复操作、职责边界和可使用性应能由实际行为证明。

## 11. 旧失败、新失败和验收真实性

- **C01**：P09 新纯验证/存储已完成；旧产品完整布局恢复仍 FAIL，直到原 P12 新 ApplyLayout 接管。P10 的 Host 能力测试不是更改旧失败判定的理由。
- **C03**：旧命令回调代码保活仍归 P11；不能把新 Host/通知的生命周期问题挂旧编号延期。
- **C04**：旧 bootstrap 连接失败仍归 P12；新工厂/挂载失败是 X10 自身必须正确的责任。
- P05 冷构建原失败和 P06 窄生成依赖修复记录继续保留。若 P10 碰到新的真实生成顺序问题，应保存首次失败并在所属窄依赖修正，不声称重跑后等同首次冷构建通过。
- P09 R1 不授权覆盖既有 recovery/marker。P10 应使用测试自有根目录，不用用户项目作为故障清理对象。
- 用户 ProjectBuilder.cpp 差异不计入资格实现，不因工作区保护而伪称整个本地树干净。

历史测试的原正确语义必须保留，但阶段迁移可以调整其 API 调用并建立明确映射。原只读冻结报告不改写；当前门禁按实际阶段和真实代码检查，不能把 P10 门禁退回 P09 求 PASS。

## 12. 阶段收据与交付门槛

至少交付以下材料，全部绑定最后测试过的实现 SHA：

| 材料 | 内容 |
|---|---|
| `dev_log/P10/README.md` | 七项 X10/相关 Q、实测范围、旧失败和新缺口、阶段状态 |
| receipt / commands | 准确实现SHA、最终配置、执行命令、退出码、相对归档路径、哈希 |
| 当前迁移账本冻结 | 逐成员/逐文件的新owner、算法唯一位置、已删/暂留及限定消费者 |
| 功能矩阵 | Scene/Material/Flow/辅助工具原入口到新入口的覆盖及不可遗漏行为 |
| owning/生命周期记录 | Host/Pane/子控件、Session/Run、投影/视口、任务/完成、代码/结果的寿命关系 |
| 新 harness 实际依赖图 | 无旧大类、私有include泄漏、静态/安装传递边；负例和修复后正例 |
| GPU/输入证据 | 新双视口及原显式模式分开，验证层与输出、焦点/capture/IME范围准确 |
| 安装资格 | 重装SDK后新模块的实际consumer，不从源码/构建树借未安装资源 |

全量构建与第二次无工作、Editor 全回归、PLAYER、原13组SDK及新增必要消费者、八项 operation 编译负例、两项原显式GPU模式继续执行；**180/93 是前置基线，不是本轮需要凑到的最终总数。** 新宿主/视图测试按事实登记。不要因为旧测试仍需旧产品，便让新 harness 链入旧框架。

最终配置和审计显式使用：

```text
LUX_EDITOR_MIGRATION_STAGE=P10
```

保存失败日志和开发期未成功资格，不混作最终结果。归档可移植性继续检验生产路径不可用、必需日志缺失、内容篡改。实现与验收分别提交，正常推送，不 force-push、不修改 main。

## 13. 可直接交给实施方

> **P09 R1 复审通过，允许进入 P10，仅执行 P10。**
>
> 以 `3363f83dcc448db9e79cea97c5176a546c05cf5a` 为已验收前置，核对真实分支、工作区和祖先关系。保护 ProjectBuilder.cpp，不 reset、不修改 main。
>
> 读取原 V4 P10、本启动补充和当前施工账本。按 A–G 内部顺序完成正式 ViewHost/DesktopShell、Scene/Material/Flow 新视图、任务/项目/Inspector 等辅助工具和真实双视口切片。新功能来自正式模块，harness 仅装配测试，不安装第二产品，不提前做 P11动态扩展或P12入口切换。
>
> ViewHost 独占顶层Pane；SessionStore/RunStore/保存与编译服务保持原权威。视图仅组合明确绑定、视图本地状态、窄访问、交互和本视口呈现；不拥有作者源/History/checkpoint/SaveOperation/RunSession，不以无效key伪装Unbound。
>
> 工厂只产生完整DetachedView；Host准备并在安全点接管，通知后结构请求排下批。重绑定失败保留旧绑定和相机；关闭视图不隐式关闭会话，GPU/任务/退休结果继续由原owner结清。继承历次错误分类、可靠完成和唯一公共控制owner，不用平行busy或通用Context补偿。
>
> 按原P10逐文件提取editor/ui的平台、领域与通用算法；迁出原体删除，新旧只共享唯一实现。旧editor_ui/旧工具的必要转换壳限定消费者到P12，新harness不得经任何链接或私有头依赖它们。ViewInfo继续使用contracts唯一定义，目录按工具主题收拢。
>
> 执行X10-01～07、相关Q、原180项行为/历次R1R2、PLAYER、SDK、八项编译负例和两项原显式GPU模式。**本阶段必须执行新SceneView真实双视口GPU/验证层，不可拿旧模式或mock替代。** 输入/capture/焦点走新桌面；系统IME实测与未测分开，缺必测只报PARTIAL/BLOCKED。
>
> P09布局仍只组织视图；内容恢复来自独立清单，不解析所有布局opaque发起open/rebind，不覆盖既有marker/recovery。C01/C03/C04保留原责任，P10新缺陷不能借旧编号延期。
>
> 唯一可变材料仍为`.internal/editor-redesign/`，阶段末冻结`dev_log/P10/`。显式P10门禁，分别提交实现和验收、正常推送，停在P10等待复审，不自动进入P11。

## 14. 源码与规范索引

下列为本文件依据。固定源码使用本次验收SHA；源码事实、提交报告和建议已分别在正文标明。源文件行号不以连接器JSON行号代替真实源行号，按文件/符号定位。

- **[P10]** 原 V4 `phases/P10_views_and_desktop.md`，包内 `reference/v4/phases/P10_views_and_desktop.md`。原件从已提供V4压缩包原字节复制，本文件不改其内容。
- **[S01]** [本次验收提交及父链](https://github.com/LUX-YU/lux-engine/commit/3363f83dcc448db9e79cea97c5176a546c05cf5a)。
- **[S02]** [P09-R1 README](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/README.md)。
- **[S03]** [LegacyWorkspaceImporter.cpp](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/editor/workspace/storage/src/LegacyWorkspaceImporter.cpp)，`prepareLegacyMigration/continueMigration`。
- **[S04]** [workspace.cpp](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/editor/tests/workspace/workspace.cpp)，`legacyPair/scopedRecovery/selectedLegacy/selectionFailures/resumeSelected`。
- **[S05]** [修复前同key冲突](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/before/r1-alpha.log)。
- **[S06]** [修复后同key实际SDK结果](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/logs/sdk-workspace-r1-alpha.log)。
- **[S07]** [修复前不同key错误并集](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/before/r1-extras.log)。
- **[S08]** [修复后不同key实际SDK结果](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/logs/sdk-workspace-r1-extras.log)。
- **[S09]** [CTest归档](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/logs/ctest.log)，本次读取末段汇总和新增场景。
- **[S10]** [check_receipt.py](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/dev_log/P09-R1/check_receipt.py)，已阅读主要检查逻辑；本次未执行。
- **[S11]** [当前旧editor_ui构建](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/editor/ui/CMakeLists.txt)。
- **[S12]** [当前IViewHost/DetachedView](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/editor/views/api/include/lux/engine/editor/views/IViewHost.hpp)。
- **[S13]** [当前contracts中的ViewInfo](https://github.com/LUX-YU/lux-engine/blob/3363f83dcc448db9e79cea97c5176a546c05cf5a/editor/contracts/include/lux/engine/editor/views/ViewInfo.hpp)。

## 15. 本文件的使用边界

P10-A需要进一步阅读实际Root、平台输入/输出、各工具UI和生成器全调用面。本次启动审阅没有重新穷举这些旧实现，也没有声称第8节已完成逐引用迁移。因此施工时用当前账本/源码核对符号，不用猜测填补。

本文给出的是清晰的实施目的地和必要接口契约，不要求机械照抄未验证的代码草图。任何改变职责/阶段范围的决定写入同一施工记录；允许合理的局部实现选择，不允许将缺失功能、测试不足、旧依赖或新重复状态隐藏在“实现细节”中。
