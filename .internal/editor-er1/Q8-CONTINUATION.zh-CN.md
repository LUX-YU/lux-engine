# ER-1 q8 续行：实现与验证边界

本轮保留 q7 架构与 G01—G04；只推进本次批准的 EX-04/05、Q7-UI-CONSTRUCTION-1、P02 和原资源门槛。ER-1 尚未完成，正式入口删除条件未满足。最终资格、成本与源码/产物身份另见本轮结果报告；此文件不预告未完成测试通过。

## 授权 diff 映射

| 范围 | 实现位置 | 约束 |
|---|---|---|
| EX-04 / UI-CONSTRUCTION | modules/function/ui/UISession、UiFontSource、私有 FontValidation | 冷创建工厂，独立持有字体 bytes/ranges；Impl 析构准确销毁 context；准备失败恢复此前 current context；atlas 复制错误在所属 DLL 转换 |
| EX-04 / 显式装配 | editor/ui/shell/WindowSpec、EditorWindow；scene_workbench/main | --font 显式读取现有文件，32 MiB 限额，不查找/安装/分发字体；文件/字体错误分别保留 |
| EX-05 | UiTextInputAnchor、UISession；EditorWindow.cpp | 通用 UI 仅输出当前已捕获帧的 caret/line-height；IMM 完全位于 EditorWindow 私有 Windows 路径 |
| P02 | SceneResources.hpp/.cpp；IdleSceneCost.cpp | 完整 Entity 代次定位当前 owner request；源身份仍在 request key；旧请求继续原退役路径；不复制第二份权威状态 |
| R01 | SceneSession::openInspection；SceneGpuTest resolved_source | ResolvedMeshResources 与 Mesh3D 来源冲突在接纳前返回 STALE_CONTENT，保留 Scene/选择/输入 |
| R09 | SceneGpuTest；诊断 RendererTestAccess/SceneTestAccess | 在真实 consumer tick 边界暂停；真实容量2的 Control/Upload拒绝；原请求恢复与关闭分别验证；正常产物不含暂停/计数路径 |
| 流程 | SceneGpuTest、RunEditorEr1Qualification.ps1 | 全局 PASS 移到所有业务及关闭检查之后；全量构建失败不启动测试；clean clone 内容 SHA 校验防止 mtime 伪 no-op |

## 构造与分配边界

UISession::Impl 构造不再使用错误的 noexcept。context 一旦创建，Impl 即负责其清理，外层 UISession 尚未构造完成也会清理。context 先销毁，再释放字体与 range 成员。原 current context 由配对 lease 恢复，嵌套锁为原有递归 mutex。

诊断替换 new/delete 实际链接在专用 ui.dll；EXE 中 ImGui allocator 包装只计数、不注入失败。修前 indices 7—10 的真实 std::bad_alloc 留下18个 ImGui分配；原UI仍可用并不能证明无泄漏。indices1—5进程异常不计为准确负例通过。修后同点必须兼具分配对消、原context恢复、A和重试B可用、输入不变。

字体数据限定为可信的现有 TrueType/TTC，检查 SFNT envelope、face、range、尺寸和 atlas 上限。此校验不是任意恶意字体的完整 sanitizer。第三方 ImGui/stb 的 C malloc 故障未注入；不能把本项目 C++ 分配点扫描称为覆盖第三方所有分配。正常默认字体构造仍兼容；显式字体采用结构化工厂。

## Unicode / 字体 / 锚点

使用系统已存在 C:/Windows/Fonts/msyh.ttc，face0、18 UI逻辑像素；ranges U+0020—007E、3000—303F、4E00—9FFF、FF00—FFEF。原文件不进入源码或SDK归档。实际文件 hash 在外部资格记录中保存。

辅助测试向通用 UI 发送选定 UiText，检查 UTF-8 E4BDA0E5A5BDE38082EFBC8C41（你好。，A）、实际 glyph 和 fallback、caller backing 释放后字体有效、另一个UI会话隔离、帧与失焦/隐藏失效。它不证明 Windows 的 n→i→候选确认正确。

本轮 Computer Use native pipe 连续失败，重建会话后仍不可用；人工输入请求未获得结果。未取得真实 q8 WindowTextEvent/UiText/Filter 三层链、候选画面或非100% DPI回归。此前 q7 的问号现象仍不能仅凭辅助 glyph 测试归因为字体。选定文字诊断只在专用构建、显式环境变量启用时输出有限固定测试字符，不收集任意用户输入。

锚点是主 viewport 相对 UI逻辑坐标，仅在成功 captureFrame 后发布；beginFrame、失焦和失败时无效。EditorWindow 按实际 GetClientRect / 本次 FrameInfo extent 转换一次，额外 framebufferScale 不参与；同时设置 composition 和 candidate，配对 ImmGetContext/ImmReleaseContext。实际 Win32调用成功与候选位置仍待真实验证。公开接口没有 HWND/ImGuiContext。

## 未达到的原门槛

实际 thread/device/attach 分阶段失败、独立 material 上传失败、set_stopped/value各自真实路径、shared-import/preserve实际GPU、持续RMB/MMB跨失焦/隐藏/关闭、部分关闭借用/owner访问器线程、匹配resize/retry尾部成本尚不能由本轮新增测试替代。保留旧正式入口；Text/Record不是材质迁移资格。不进入ER-2，不推送/合并/发布/冻结。
