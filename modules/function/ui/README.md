# UI：Root、Pane、Element 与 Layout

`ui` 提供 CPU UI；`ui_rendering` 提供可选 RenderFeature。UI 不拥有原生窗口、Scene 或 GPU 资源。

Root::create 先完成私有 Context/Impl，再构造 Root 和注册 ObjectId。配置或后端失败不发布对象身份，
已有 ImGui Context 保持不变；成功 Root 在整个寿命中始终拥有完整后端，没有 initialize 或空壳状态。

## 所有权与身份

Root 是全部窗口的唯一 owner，使用 SlotKeyAutoSparseSet<PaneId, unique_ptr<Pane>>。
Pane 直接属于 Root，没有嵌套 Pane；停靠、标题和显示状态不改变 owner。
Pane::addElement(Element&) 设置唯一内容根，通常为 Layout；第二次添加返回 OCCUPIED，替换用 replaceContent。
Layout::addElement/replaceElement 只建立非拥有关系。addElement 与 Object addChild 一样允许 reparent，完整准备后迁移；
失败保留原树，迁移撤销旧窗口焦点、捕获和菜单目标。固定内容优先值成员，动态内容由具体 owner 保存 unique_ptr。
叶子控件拒绝子内容；通用 LuxObject 接口不能绕过 UI 拓扑。父对象析构只解绑剩余外部内容。

Pane 只用标题构造；Element、Layout、控件可离树构造。ObjectId 是进程身份；PaneId 是当前 Root 内一次登记。
移除后重加取得新 PaneId，同标题窗口可以并存，ImGui 标签使用登记身份。持久名称属于 Editor。
PaneId 由 Root 私有使用；普通调用通过 Pane&，枚举使用同步 forEachPane，不暴露容器 owner。
内部跨帧目标同时保存 Root ObjectId 和 PaneId，执行前验证代际。Element 的 ImGui 身份来自 ObjectId，无字符串 ElementId。

addPane(unique_ptr<Pane>&&) / addPanes(span<unique_ptr<Pane>>) 完整验证候选、准备容量后才转移 owner，最后通知。
失败保留全部候选和原 UI。removePane 撤销焦点、捕获、菜单和待执行结构操作，使 PaneId 失效，通知后返回 owner。
clearPanes 在全部登记撤销后通知和析构。容器 swap-and-pop 不执行用户析构。

replacePanes(remove handles, add owners, on_commit) 使用同一挂载/卸载内核完成原子替换。
失效 remove handle 忽略，重复有效 handle 拒绝；按最终窗口数验证容量，失败不消费候选或修改原树。
全部旧窗口脱离、新窗口挂载后，先同步调用一次 noexcept on_commit，再发送移除/添加通知。
on_commit 只提交上层已准备好的状态，不分配或重入结构操作。返回旧 Pane owners，由调用方在依赖仍有效时销毁。
Root 不知道 Project、Session 或业务分组；应用只保存自己的非拥有 PaneHandle。

## 构帧和安全点

私有 detail::Context 保存 ImGuiContext、字体/主题、输入积累和 EventId 对照，并执行后端构帧及捕获。
Root 保存窗口、焦点、捕获、模态、停靠和内容树编排，不公开 ImGui ABI。
update() 只维护；update(FrameInfo, DrawData&, optional<Capture>) 执行绘制/捕获、同步资源固定回调、已处理输入路由和树维护。
回调不跨帧保存，Root 不认识 Renderer。空输出时仍维护和处理失焦，不重放控件交互。隐藏窗口仍维护。
父先子后遍历 Pane→Element 子链，没有全局 Element 登记或逐帧目标快照。

维护、绘制、测量、排列、事件和通知期间冻结结构。deferChange 保存 Root/Object/Pane 代际身份与操作指针，
输入保留在具体 owner。结构修改与命令执行共用一个 Root 安全点队列，按入队顺序执行。
update 开始时固定本批长度；相同目标/操作的待处理结构意图合并，命令不合并，回调追加的工作留到后批。
卸载或析构统一撤销两类旧意图，执行前再次验证身份和所属窗口。失效目标不改投全局命令接收者。
一次同步操作的完整调用链保持目标借用保护；owner 可以在安全点替换子内容，不能销毁当前回调对象或祖先。

## 输入、布局与控件

feedInput 保留顺序和 sequence，返回 FULL/CLOSED/INVALID_INPUT；FULL 不消费事件或序号。
只路由 ImGui 已消费的 trail，不提前派发 trickle 队列。Composition 表达 IME 状态，Text 是唯一文字路径。
捕获失效和失焦送达实际交互 owner。模态窗口阻断外部路由；无新帧时仍处理失焦。

Layout 支持水平、垂直、网格和表单。隐藏内容不占空间，宽度先确定再测量换行高度；
尺寸遵从最小/期望/最大及伸展权重。空间不足时裁剪或显式滚动。控件每帧只实际执行一次。
UI 只提供 DockTree。setDockTree 在内部验证、准备并提交；树中保存 opaque PaneHandle，移除后重加不会复活旧布局目标。
Editor 把稳定名称解析成当前窗口，不保存 ImGui runtime ID，不将五区产品偏好放入 Root。

Button、Label、TextEdit、CheckBox、NumericEdit、Choice 是 Element。setValue 不发送用户编辑信号。
用户交互报告 EditResult，finishEdit 支持无新帧提交/取消，文本 Undo 优先由控件处理。
Pane 没有 draw/drawContent 覆写口；closeRequested 只表达意图，不自动隐藏或销毁。

## DrawData、资源和菜单

DrawData 拥有顶点、索引、命令和图像 ID，复用缓冲，不传 CPU Context/Pane 借用到渲染线程。
字体 atlas 是拥有型像素副本。ImageElement 只显示非拥有 TextureHandle，不读资产或 retain/release GPU。
业务 owner 和发布 owner 分别保证资源需求与已捕获帧使用责任，退休仍由原渲染链处理。

setMenu 接收拥有 CommandId 和标签字符串的值树，不保存外部 source token，复用 Command QUERY/EXECUTE。菜单打开时固定目标；实际执行离开绘制栈，
验证对象及窗口代际，不改投新焦点窗口。业务身份和历史校验属于 Editor。

测试覆盖布局、控件输入、固定批次、所有权、拓扑、代际及同步回调冻结。
CPU 模拟输入不代表系统 IME 或原生输入接管资格。

## Generic commands and measurement

Concrete controls use protected `Element::menuActive()`; Root does not friend particular controls. Menu items own
labels and CommandId. Shortcuts are supplied by the host and follow the same query/deferred-execute path, preserving
focus and stale-target checks. Root has no implicit Ctrl+Z/Ctrl+Y policy or MenuRequest host hook.

`setCommandFallback(LuxObject*)` keeps only the object's generational ObjectId. Both QUERY and EXECUTE first route
through the focused Element/Pane and Root boundary; only unaccepted requests reach the fallback. With no focused
target, application commands can still be queried and queued for the next safe point. A removed original target
cancels its queued command; it does not become a new target-less application request. Clearing or destroying the
fallback never leaves an owning pointer or a stale-address callback.

`Root::statistics()` is the latest update snapshot, populated by the actual hierarchy/draw phases. Hidden content
continues to count and maintain; maintenance-only updates report zero draw/capture counts. Theme application belongs
to Context creation; normal beginFrame does not rewrite the scaled style. Event routes local input/commands up the
tree; Signal carries semantic notifications from a shared domain owner. Use neither to stream per-frame bulk data.
