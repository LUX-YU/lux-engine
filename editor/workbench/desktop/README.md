# Desktop

view_api provides detached ownership and the narrow host protocol. ViewInfo remains an Editing value;
ViewError owns close diagnostics here. ViewHost is the only top-level Pane owner; Root borrows its nodes.
desktop_shell composes the existing UI, platform and renderer frame boundaries without owning author sessions.


## Desktop and view ownership

`view_host` owns complete `DetachedView` units. `Root` registers their UI trees but does not own them. A factory must not register a window or adopt a Session. `desktop_shell` supplies the shared input, Root, DrawData and UI rendering path; it neither drives the SceneRuntime nor owns editing, saving or compilation services.

The application collects accepted background completions, updates the shell, drives its one SceneRuntime, and submits the existing renderer work. The shell fixes DrawData resource uses before view maintenance can replace outputs. With no writable UI frame, maintenance and retirement still proceed.

Host adoption prepares capacity and routes before committing. The host takes the candidate only after preparation succeeds. Notifications run after the structural commit; requests made in callbacks enter the next bounded batch. `ViewAdoption` and `ViewDrain` preserve notification delivery results. Native close intent is coalesced in its existing slot, so a full external queue cannot discard it.

Each owned unit keeps its defining code alive until its Pane and all member/unique-owned children have been destroyed. Closing a view first finishes its `prepareClose` contract, then detaches its routes and focus, then destroys the unit outside callbacks. Temporary BUSY keeps the unit intact. Permanent refusal retains the diagnostic and stops automatic retry until explicitly requested. Destruction requires an outer safe point and dependencies that outlive the host; it does not synchronously wait for GPU or executor work. Accepted work belongs to its service, not the disappeared view.

### Related formal modules

| Module | Owns | Borrows |
|---|---|---|
| scene | SceneView camera, Inspector display/interaction buffers, viewport request | SceneSession access, Run inspection, interaction group, shared projection |
| material | Graph/slot display state and preview viewport | MaterialSession access, interaction, MaterialCompilationService and preview owner |
| flow | Graph/property display, last compilation ID | FlowSession access, interaction and compilation service |
| tasks | Task list display | the shared activity TaskMonitor |
| project | Catalog display and pending asset request | ProjectCatalogModel and an explicit AssetReference intent receiver |
| widgets | Virtualized tree/node canvas algorithms | UI callbacks only |

`SessionStore` remains the sole author Session owner. `RunStore`, SaveService and compilation services retain their original completion and acknowledgement duties. Views own no author source, History, checkpoint or SaveOperation. Layout application organizes existing views; it does not inspect opaque layouts to open content or rewrite recovery/marker files.

The integration executable assembles these formal modules for qualification; only EditorApplication is
installed as the product. No old Context or tool Editor is linked. Inspector generation has one
implementation, producing author and runtime controls against their distinct field capabilities.

P10 native input tests use actual OS mouse/keyboard events through the new shell. They do not certify system IME candidate composition; that scope must be recorded separately. The installed product uses the same desktop and V7 contribution contracts.

## View API（P08）

窄公共契约只链接 UI 和 contracts。ViewId 是 host 域、槽位和代次；恢复键独立；ViewTypeId 复用 PaneTypeId。
Host 为一次挂载分配 PaneId，不能把稳定恢复键当成 live ViewId。ViewInfo 拥有字符串，不暴露集合。

DetachedView 是 code + unique_ptr<Pane> 的唯一 owning 单元，移动赋值以完整单元交换，旧节点先于旧 code 清理。
必须离树构造；挂载后由 Host 保持该单元，完成卸载和资源责任交接再析构。直接析构仍挂载的单元是契约错误。
Pane 和控件成员／unique_ptr 各自只有一个 C++ owner；Root 和 LuxObject 父子链只观察。

ViewRequests 的 close/show/focus 接收 ViewId，宿主排队，在安全点重新检查代次。当前回调不得删除自身。
真实 Root 协议由同目录的 ViewHost/DesktopShell 消费；FakeHost 仅用于协议单测，不代替桌面验证。实际产品入口使用同一正式 DesktopShell。

`test/lifecycle.cpp` 同时用于安装消费者，执行真实 ImGui Root 的绘制、注册、焦点撤销、离树析构与通知故障。
这不是 P10/P13 GPU 像素、IME 或新产品资格。
