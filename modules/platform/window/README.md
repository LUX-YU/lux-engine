# Window

LuxWindow 管理原生窗口并记录输入事实，不解释 UI 或游戏业务。
drainInputEvents 返回按原生窗口单调编号的固定输入批次；后来的回调进入另一份复用缓冲，不改动当前借用。

Windows 通过原窗口过程的 subclass 记录 IME STARTED/UPDATED/COMMITTED/CANCELLED 状态。
失焦取消尚未提交的 composition；提交文字只由 GLFW 字符回调产生。
不额外读取结果字符串，不重复提交字符，候选窗口仍由 Windows 管理。
subclass 在原生窗口销毁期间解除，晚到消息不会持有已销毁的 LuxWindow。

window.input_batch 验证顺序、序号、缓冲复用和 Windows composition 消息；真实输入法候选位置与 UI 行为仍由桌面验收覆盖。
