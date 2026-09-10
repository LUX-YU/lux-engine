# ER-1 固定候选人工桌面验收记录要求

当前没有 q9 最终正常/诊断资格产物，本文件是待执行的验收步骤，不是运行结果。
q7 的已知 IME 失败未关闭。不得用自动 UiText/glyph、粘贴或 Computer Use 服务故障代替下列桌面结果。

1. 使用同一新候选的 qualification.json / diagnostic-qualification.json；逐一比对 exe、UI、Window、Renderer
   DLL 的 SHA256 和 source_commit，分别记录正常与诊断配置，限定 RelWithDebInfo。保存显示器分辨率、
   Windows DPI、键盘/IME 名称及版本、字体文件哈希。只使用本机已有的显式字体路径，不搜索、安装或分发字体。
2. 从该候选实际 bin 目录启动 `lux_editor_er1.exe --validation --assets <固定 pak> --font <已有字体>`。
   不传 --frames，不用隐藏窗口，不设置自动结束程序。完整重定向 stdout/stderr 至新的独立日志目录。
   运行中的 exe/DLL 仍按 manifest 核对，不能在旧程序已启动后替换 DLL。
3. 鼠标真实点击局部文字编辑域，逐键 **n → i → Windows IME 选词“你”**，录屏包含候选框、光标及最终文字。
   在同候选诊断产物日志查实际 `UiText` codepoint 与内部选择文本/glyph；“你”应对应 U+4F60。
   画面显示、内部字符和日志要属于同一操作/同一进程。单独看到问号不能归因为缺字体。
4. 移动窗口、在已记录 DPI 的屏幕上再次操作，记录候选框是否跟随实际输入锚点；失焦、返回、隐藏恢复、
   局部文本 Undo/Redo 各自实测。局部文字操作不得改变 Scene 的 selection/history 或另一个文档历史。
5. 在 Scene viewport 持续按住 RMB/MMB，越过 Pane 边界、窗口边界、失焦及隐藏/恢复；实际释放并再次进入。
   记录捕获、相机是否继续错误移动、其他 Pane/文档是否被误操作。滚轮用例不替代持续按键捕获。
6. 从真实窗口关闭入口结束。保留完整终态 `ER1 seconds=... leases=0 views=0 accepted=0`、退出码及
   descriptors 创建/退役配对；非零错误、缺失终态、超时或被强制结束均不写正常退出。
7. 每一项填 PASS/FAIL/UNVERIFIED，并附实际视频/截图、输入顺序、内部值、进程和文件身份。
   如果无法读取所需内部值，该项保持 UNVERIFIED，不能从画面或别的进程日志猜测。

不要将手工运行准备完成、程序成功启动或辅助测试通过标为真实 Windows IME 验收通过。
