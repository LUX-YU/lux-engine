# Window

LuxWindow 管理原生窗口并记录输入事实，不解释 UI 或游戏业务。
drainInputEvents 返回按原生窗口单调编号的固定输入批次；后来的回调进入另一份复用缓冲，不改动当前借用。

GlfwRuntime::create 返回唯一的平台线程 owner，重复创建返回 ALREADY_ACTIVE；失败不发布 owner，允许下一次重新创建。
owner 必须覆盖所有原生窗口及使用 GLFW 的等待/唤醒。查询 Vulkan 扩展列表不再隐式初始化平台。
LuxWindow::create 先准备原生窗口和可失败的回调登记，再构造完整对象；没有 init、isInitialized 或重试空壳。
派生工厂使用受保护的 prepareNative，候选自己拥有回滚责任，完整构造后转交同一份原生资源。
Android 的桌面工厂明确返回 UNSUPPORTED_PLATFORM，活动原生 surface 仍由原 Android adapter 管理。

Windows 通过原窗口过程的 subclass 记录 IME STARTED/UPDATED/COMMITTED/CANCELLED 状态。
失焦取消尚未提交的 composition；提交文字只由 GLFW 字符回调产生。
不额外读取结果字符串，不重复提交字符，候选窗口仍由 Windows 管理。
subclass 在原生窗口销毁期间解除，晚到消息不会持有已销毁的 LuxWindow。

window.input_batch 验证顺序、序号、缓冲复用和 Windows composition 消息；真实输入法候选位置与 UI 行为仍由桌面验收覆盖。

Desktop placement uses signed window content coordinates, separate from framebuffer pixels and content scale.
`resolveWindowPlacement` is pure and does not query a monitor. A persisted display name is disambiguated by
its work area; disconnected displays fall back to a usable primary work area. Explicit invalid sizes are
rejected, while stale saved rectangles can be repaired. Tiny work areas degrade to positive content sizes.

`LuxWindow::displays`, `state` and `applyPlacement` are owner-thread platform operations. Ordinary restore
rectangles survive maximization and fullscreen. `applyPlacement` reports observed state; hosts must compare
actual facts with their desired values when the window manager constrains a request. Placement notifications
are coalescible: inspect the facts at the host's safe point, never persist from the native callback. Display
hotplug advances `GlfwRuntime::displayRevision`; this does not require enumerating video modes every frame.
The Android stub reports these desktop operations as unsupported.

`window.placement` is the pure CPU policy test. `window_placement_test --desktop` is an explicit real-window
qualification, separate from native keyboard/mouse and IME testing.

`window.lifecycle` 验证真实 owner 实现的构造失败、唯一 Runtime 和精确释放次数。
`window_lifecycle_test --desktop` 另外执行隐藏的真实 GLFW 窗口、原生创建/Win32 回调故障注入、派生候选回滚及重建。
故障接线只编入测试目标；正式 DLL 和安装头没有测试开关。自动 composition 消息不代表系统 IME 人工资格。
### Tray registration

`TrayIcon::create(window)` either returns the complete Win32 tray owner or an exact native-stage error.
Menu, shell registration and subclass belong to that instance. A second tray on the same window is rejected;
different windows are independent. The owner never changes GLFW's native userdata or replaces its window procedure.
The previous exit policy is restored on tray destruction; while the tray is attached its close policy is HIDE.
Destroying the native window first revokes the callback and shell registration. A reentrant menu callback retains
only its physical state until return, and cannot use the revoked window. The stock Windows icon is borrowed.

`window_tray_lifecycle_test` uses production source and native failure injection. It is registered only in the
explicit desktop test mode. The window-only installed consumer separately exercises the actual SDK DLL.
