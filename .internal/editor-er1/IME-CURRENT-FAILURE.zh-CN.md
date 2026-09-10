# q7真实IME交互：已执行且未通过

实际正常候选：2d2650c5d498d4c74b0ba076dd3f5b322b6c13f7，E:/lux-er1/q7/build/bin/lux_editor_er1.exe --assets E:/lux-sv1/sv1.luxpak --validation。EXE SHA256/PID/开始UTC见review-github-01/gui-q7-identity.json。

本次通过computer-use @oai/sky实际启动并观察窗口，鼠标聚焦Outliner Filter，再分别发送真实键盘n、i，观察Windows中文IME的ni候选，发送数字1选择可见的“你”，最后BackSpace清除。没有用type_text粘贴中文替代IME。每次操作都刷新实际目标窗口；初始场景、候选、确认及清除的截图/可访问性/UTC在gui-q7/actions.json与PNG/JPG。

实际结果：
- 原场景3资源READY，Cube被选中，Inspector有对应字段，Mesh/Light画面可见。
- IME组合ni和中文候选实际出现，候选条位于窗口左上方，而非Filter光标附近。
- 数字1选择候选后，Filter显示“?”，列表因非匹配文本清空；BackSpace后原列表恢复，选择和Inspector保持。相机画面在此输入过程中无可见移动。
- 此结果证明实际候选选择和过滤行为发生，不证明字符串内部编码或汉字显示正确；问号的精确原因仍需修复取证。源码UISession只建立默认字体atlas，没有公开字体配置；该事实支持缺字形方向，但不能用源码推导代替已确认Unicode字节证据。
- Alt-F4正常显式关闭：exit0，ui_iterations21364，frames21375，descriptors2/2，texture_misses/events/dropped/validation_errors均0，leases/views/accepted均0；原始stderr日志保留。

U12保持PARTIAL，并新增明确ACTUAL_FAILURE事实，不称“IME环境未知”或PASS。持续RMB/MMB跨失焦/隐藏/关闭仍未执行：本次运行时可初始化，但受支持DragInput没有按钮/保持参数，也无mouse-down/up接口。此为API能力限制；computer-use要求仅用该JS API进行Windows自动化，不通过额外SendInput/自制helper旁路冒充完成。人工步骤见MANUAL-INPUT-ACCEPTANCE.zh-CN.md。

## 最窄后续变更边界（尚未实施/批准）

原用户批准执行的v4 §3.3将modules改动限于五项：layout、NodeCanvas、外部纹理/cleanup、UiFrameSnapshot、业务prepare/replace；已追加的EX-01/02/03分别针对allocator、RenderGraph关联、reply预算。字体和IME接口不在这些条目中。

已核对现有接口：modules/function/ui/include/lux/engine/ui/UISession.hpp的UISessionCreateInfo仅有Theme；pinclude/.../UISessionPresentationAccess.hpp仅有captureFontAtlas；UISession.cpp构造时直接默认GetTexDataAsRGBA32。没有可从EditorWindow调用的字体装配或输入光标位置值接口。不得借captureFontAtlas.context把完整ImGui上下文暴露给Window来绕过职责。

拟限于以下具体改动，不重新选择架构：
1. 通用UI的冷构造参数接受有限的拥有型字体数据/字形范围，构造阶段完成验证与atlas准备，Renderer仍只消费既有拥有型atlas快照。实际测试字体来自本机既有系统字体并记录hash，不升级依赖、不打包未授权字体、不做运行中热替换。
2. 通用UI只输出当前文字输入是否活跃、caret矩形等平台无关值；平台IME设置留在engine/editor/ui的Windows私有实现，由EditorWindow在owner线程调用，不把HWND/Session/Editor语义放入modules。
3. EditorWindow/Application显式装配字体和窗口IME接线，真实Filter作为消费者。补字体读取/准备失败保留owner，重复真实ni候选、确认字形、输入期间相机不动作、关闭计数；重新形成新tracked候选并执行受影响正常/诊断与安装资格。

需要批准的范围仅为上述通用UI字体冷配置与IME caret值接口及对应Editor接线。当前q7产物保持原样，仍带已记录的界面失败；其余尚缺启动/共享GPU/资源背压等门槛单独保留，不把它们归因于IME范围限制。
