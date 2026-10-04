# Desktop

## EC4 composition

`editor_composition` provides `EditorContext` and the immutable `UiRegistry` factory catalog. It depends
on the neutral services, UI and command providers, with no concrete tool or old Host dependency.
The existing `CommandMenu` belongs to this same provider. Menu requests, programmatic commands and
configuration batches can consume the same fixed `UiHandle`; BUSY retains queued input and replacing
the catalog rejects a stale handle instead of silently resolving its name again. The installed
composition consumer exercises this route without importing desktop_shell or view_host.
Factories return complete detached unique owners. `Root::addSubPane`/`addSubPanes` register them and can
transfer ownership into the existing LuxObject parent relation; Root has no second owning window table.
External members and externally owned children remain external and are unlinked when their parent ends.

Content selection uses a declared set of author kinds and an optional preferred window type. Resolution
returns a fixed `UiHandle`, rejects ambiguity and missing preferences, and never constructs a service or
window. Dynamic declarations freeze their kind names in the same backing as their other metadata.
Creation verifies that the selected entry still belongs to the accepted catalog. Configuration batches
retain their captured input, service definitions and Root version through preparation and cleanup.

`SessionStore` keeps the sole logical identity/close authority while its actual allocations are shared.
Holding a shared allocation does not restore a closed SessionId. Services and views keep their original
completion, source-stamp, publication and retirement duties.

## Product protocols awaiting M6 migration

The following Host paths still serve the existing Application and tool callers. EC4 removes them in M6;
they are not the ownership or factory contract for new composition consumers. The old ViewFactory calls
the same private content-selection algorithm as UiCatalog, so it does not retain a second selection policy.

view_api currently provides the detached unit and host protocol. ViewInfo remains an Editing value;
ViewError owns close diagnostics here. ViewHost owns these old product units; Root registers their nodes
as external children.
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

`SessionStore` remains the sole author Session identity/close authority. `RunStore`, SaveService and compilation services retain their original completion and acknowledgement duties. Views own no author source, History, checkpoint or SaveOperation. Layout application organizes existing views; it does not inspect opaque layouts to open content or rewrite recovery/marker files.

The integration executable assembles these formal modules for qualification; only EditorApplication is
installed as the product. No old Context or tool Editor is linked. Inspector generation has one
implementation, producing author and runtime controls against their distinct field capabilities.

P10 native input tests use actual OS mouse/keyboard events through the shell. They do not certify system IME candidate composition; that scope must be recorded separately. The current Editor extension ABI is V10; V9 modules are rejected. Historical input results do not qualify the EC4 product.

## View API（P08）

窄公共契约只链接 UI 和 contracts。ViewId 是 host 域、槽位和代次；恢复键独立；ViewTypeId 复用 PaneTypeId。
Host 为一次挂载分配 PaneId，不能把稳定恢复键当成 live ViewId。ViewInfo 拥有字符串，不暴露集合。

DetachedView 是 code + unique_ptr<Pane> 的唯一 owning 单元，移动赋值以完整单元交换，旧节点先于旧 code 清理。
必须离树构造；挂载后由 Host 保持该单元，完成卸载和资源责任交接再析构。直接析构仍挂载的单元是契约错误。
Pane 和控件成员／unique_ptr 各自只有一个 C++ owner；该旧路径向 Root 登记为 EXTERNAL。

ViewRequests 的 close/show/focus 接收 ViewId，宿主排队，在安全点重新检查代次。当前回调不得删除自身。
真实 Root 协议由同目录的 ViewHost/DesktopShell 消费；FakeHost 仅用于协议单测，不代替桌面验证。实际产品入口使用同一正式 DesktopShell。

`test/lifecycle.cpp` 同时用于安装消费者，执行真实 ImGui Root 的绘制、注册、焦点撤销、离树析构与通知故障。
这不是 P10/P13 GPU 像素、IME 或新产品资格。

`CommandMenu::setShortcuts` prepares a bounded set of user overrides at a safe point. Each row carries the
stable canonical command ID, expected scope and input version; absent extensions keep their inactive rows.
Conflicting effective bindings reject the complete candidate. Fixed command declarations are never edited.
Open menus return BUSY and retain their original handles and text. A rejected registry refresh preserves
the last accepted menu and reports the incompatibility; it does not reparse the same failed version each frame.
