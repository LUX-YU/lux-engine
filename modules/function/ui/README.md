# UI：Root、Pane、Element 与 Layout

`ui` 提供 CPU UI，`ui_rendering` 提供可选的 RenderFeature 与 Vulkan 后端。Root 和 Pane 不拥有原生窗口、Scene、RenderRuntime 或 GPU 资源。

## 对象与所有权

Root 拥有 ImGui Context、字体、主题、输入、焦点、捕获和公共停靠区。Pane 与 Element 分别使用 LuxObject 的非拥有父链，两者没有继承关系。资源由成员或 unique_ptr 管理，父对象不 delete 子对象。ObjectMessageQueue 由宿主先创建、最后关闭，Root 仅接收 dispatcher。

Pane 是独立窗口，Element 是窗口内组件。Root 独立提交所有 Pane，父窗口隐藏不影响独立子窗口；停靠不改变父链。Pane::setContent 借用一个直接子 Element。Layout 通过已有子链排列内容，不增加第二份拥有型 children。Element 不进入停靠登记。

Layout 支持水平、垂直、固定列数网格与标签/字段配对的表单。隐藏元素不占空间，禁用元素占空间；表单隐藏整对，不重新配对。先分配宽度再测量换行高度；按最小/期望/最大尺寸和伸展权重计算。空间小于最小尺寸时裁剪，或显式开启滚动。DockLayout/DockState 位于 Docking.hpp，保留原有编码。

Button、Label、TextEdit、CheckBox、NumericEdit 和 Choice 是公共 Element。setValue 不发用户编辑信号；交互通过 LuxObject 信号报告 EditResult，控件缓冲不替代业务模型和历史。

原 Context、Frame、CommandRouter 已移除。Root 保留非拥有的窗口索引，每个 Pane 在构造/析构时登记/注销；它不拥有第二棵对象树。UI 复用 LuxObject 信号和栈上 EventView，不提供第二套事件系统。

## 输入和宿主循环

Root::feedInput() 接收平台无关的 InputEvent，返回 FULL、CLOSED 或 INVALID_INPUT。FULL 不接纳本次事件；调用方必须保留/重试或报告终止故障，不得静默丢失 release。输入容量在 RootConfig 中指定，ImGui 的 pending/trail 缓冲在初始化时预留。

feedInput 的可选 sequence 对应原生窗口的单调序号，零由 Root 分配。预留的关联表把序号映射到 ImGui EventId 区间；trickle 的后续输入不提前派发，FULL 不消耗序号。Composition 只报告 IME 状态，Text 是唯一字符路径。模态 Pane 不停靠，内部事件能到达所属 Pane，但不能越过它；捕获因隐藏或模态失效时直接通知实际交互 owner。

一次循环的边界为：

1. 接纳平台输入；处理已经完成的异步结果。
2. Root::update(FrameInfo, DrawData*) 一次完成绘制/捕获、drawDataReady 资源固定、剩余输入路由和树维护。
3. 宿主通过 SceneRuntime.tick 集中维护内容和 UI 场景，并由各 RenderSystem 发布；背压复用已有数据。

没有可写帧槽时传 nullptr，仍维护对象和失焦状态，不重放控件交互。
Root 不调用 Simulation，也不做渲染资源 retain/release。draw/measure/arrange/event 冻结结构。维护使用固定非拥有目标批次：析构同步将旧位置置空，本轮不复用，新对象下一轮维护；owner 可替换已结束交互的子对象，但不能在回调栈内销毁自己或祖先。

涉及业务实例采用等必须离开维护遍历的操作，由 owner 调用 `deferChange(target, apply)` 登记。
target 只能是该 Root 下已有的 Pane 或 Element；apply 是 `void(object::LuxObject&) noexcept` 函数指针，
请求输入仍保存在具体 owner 中。相同 target/apply 在待执行批次中只保留一次；不同操作不合并。
宿主在 `update` 返回后的外层安全点显式调用 `applyPendingChanges()`，它不属于 `update`，也不由资源等待调用。
执行中新登记的意图下一次采用；目标析构同步撤销当前批次与待执行批次中的记录，地址复用不会继承旧意图。
采用期间允许 owner 用普通成员替换子对象，不能销毁正在执行的目标或其祖先。
绘制、测量、排列、维护、对象事件和信号回调中直接采用属于契约错误；同步信号同样不能绕过这一边界。
队列使用复用容量的连续数组，不拥有对象，不使用弱引用或 generation，也不增加任务线程或通用删除机制。

## 绘制数据与图像

DrawData 拥有顶点、索引、命令和使用到的图像 ID，复用 DrawList/buffer 容量；没有 CPU Context、Pane 或 viewport 借用跨到渲染线程。任意带外部借用的 draw callback 在修改输出前拒绝，内置 reset callback 保留。公开类型不暴露 ImGui ABI。

ImageElement::setImage(render::RTextureHandle) 只设置非拥有身份。空 ID 输出占位，UV 与期望尺寸可配置；最终显示尺寸由布局矩形决定。组件不接收路径、AssetId 或资产读取回调。业务 owner 保证资源需求，发布 owner 保证已经捕获的图像仍有效。

ImageElement 同时显示普通纹理和离屏输出，区域几何、焦点、点击及拖放视图统一由 interaction() 提供。拖放数据只在当前帧借用，业务 owner 在 draw 中解码或复制所需值。引擎场景绑定由 L5 editor_ui 的 SceneElement 负责，资源需求由 RenderResources 管理，UI 发布捕获独立保留 GPU 使用。

字体 atlas 是拥有型 CPU 像素，GPU atlas 属于 Feature。渲染线程的 create/draw/destroy 不访问 CPU Context。Feature 会保留最近一份发布内容；Clear 结束后续绘制，已经提交的 GPU 使用仍须等实际完成，不能以 Program 被消费代替 GPU 退休。

## 维护与验证

Pane 没有 draw/drawContent 扩展口；content 是唯一绘制入口。closeRequested 只表达意图，不自动隐藏。
TextEdit/NumericEdit 的 finishEdit 支持无新帧时提交或取消；setValue 仍不发送编辑信号。
所有 Element 均有 finishEdit 入口，上层不识别具体控件类型。构造参数限定 Root→Pane、Pane→Pane/Element、Element→Element；焦点和捕获采用明确的 Pane/Element 指针，析构同步撤销，不使用 ObjectWeakRef 或 UI generation。
公开 ValueEdit 绘制 API 已删除，EditResult/EScalarEditMode 位于 Controls.hpp。

历史 CPU 探针、性能数据和 GPU/桌面验证日志已按用户要求清理。后续验收应重新采集当前实现的证据；真实 IME 和 DPI 不能由 CPU 测试代替。

专用组合 Element 若直接使用 ImGui 输出可变长内容，应在自身矩形内建立滚动区域；不能依赖 Pane 的外层滚动把内容移出 Element 的裁剪矩形。Layout 的显式滚动接口已处理这一边界。

## 菜单与布局恢复

Root::setMenu 接收 MenuItem 值树。主菜单占用固定区域，复用 Command 的 QUERY/EXECUTE，
不引入命令类层级。MenuRequest 把打开时的 Pane/Element 目标交给宿主；Editor 额外固定 HistoryId 与命令登记版本。
目标注销同步使指针失效；命令采用在绘制栈之外。无宿主处理的普通控件命令也先排队，再由 applyPendingChanges 派发。
模态窗口打开时禁用全局菜单。TextEdit 与 NumericEdit 的活动文本 Undo/Redo 使用 ImGui 编辑状态，
不能误投父级资产历史；未实现的文本菜单动作禁用，原生文本快捷键继续交给 ImGui。

DockIdentity 只将保存的窗口身份映射为当前窗口身份，包含内部角色后缀及选中标签；
DockState 仍使用原 ImGui ini 编码。Root 不认识资产、插件和磁盘设置，布局 I/O 与恢复工厂属于 Editor。

## P08 离树装配

`Pane(dispatcher, id, type, title)` 和 `Element(dispatcher, id)` 不注册 Root。
以父 Pane/Element 构造的固定成员可组成离树子树；独立 Element 可通过 `addChild` / `setContent` 关联。
关联不接管 C++ 所有权；必须按成员/unique_ptr 的逆序析构子对象。

`attachedRoot()` 返回空表示未挂载，`containingPane()` 还区分独立 Element。
离树时可设标题、modal、可见性、尺寸约束和内容；focus/capture 请求返回 false。
测量、排列、实际绘制和旧 `root()/pane()` 引用入口要求已建立相应关联，不能把离树测量伪称成功。
新工厂只传 owner dispatcher，不传 Root。rooted 构造和 root() 保留给既有产品到 P12。

Root 的 `prepareMount/prepareDetach` 只拥有一次性准备记录，不拥有节点。
整棵子树的注册容量预留在准备阶段；候选/Root 析构、子树或活动注册变更会使准备失效。
`commit` 必须在 draw、measure、update、事件和 deferred callback 之外；先完成所有关联，再通知。
提交结果含通知统计，队列 FULL/CLOSED 不能把已经提交的事实改判失败。

卸载撤销输入目标、捕获、菜单目标和延迟修改记录，再撤销注册和父链，最后发出通知。
Host 在此之前结束业务交互，在此之后按各组件原协议移交 GPU 退休责任并析构节点。
Root 不执行保存，不等待 GPU，也不接管子对象 delete。已接受的异步工作仍由原 owner 接收完成。
