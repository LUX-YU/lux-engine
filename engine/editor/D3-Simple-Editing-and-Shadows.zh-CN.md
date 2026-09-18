# 普通编辑收敛与阴影项目核对

2026-09-18，基线 `b71291f70e496a2fec72bbc7bd0641330a2265b7`，开发分支 `codex/editor-d3-foundation`。本轮按用户决定撤销两项特殊手势要求，不以它们继续阻塞 D3。

## 编辑交互

- 删除 Inspector 在拖动中捕获 Esc 并取消整个字段预览的分支。
- 删除 `gesture_drawn_` 与“本帧未绘制字段就主动提交”的专门判定。
- 普通控件结束交互后提交一次，之后通过 Ctrl+Z/Ctrl+Y 撤销、重做。ImGui 原生文本编辑行为没有改动。
- 保留已结束交互的兜底提交、切换选择/失焦收尾、失败保留 token，以及销毁 Pane 时清理未完成预览；它们避免遗留 BUSY 状态和失效 owner，不是要求用户执行特殊手势。
- 删除对应的持续按住裁剪和 Esc 取消测试要求，保留普通松手、无变化点击、Undo、Pane 析构/重建和提交拒绝重试回归。

## 阴影实际接线

Editor 的 `EditorRenderServer` 已注册 ShadowMap 和 MeshShadow feature。前向绘制仍声明阴影 atlas 采样与 shadow-view upload 依赖，没有删除此前渲染图修正。

对旧启动脚本的实际项目 `d3desktopq/manual-project/Main.luxscene`，使用正式 Pak、Scene 和 RenderSystem configuration codec 解码，得到六个 feature：view_camera、material、mesh_stack、light、forward_mesh、shadow_map，**没有 mesh_shadow**。这是实际文件检查，不只是阅读生成器推测。

MeshShadow 负责将遮挡物绘入阴影图；ShadowMap 的存在不代表物体投影已经启用。另一个示例问题是 LightDescription 默认 `cast_shadow=false`，旧 fixture 的 `gpu-shadow` 模式也未覆盖这个默认值。本轮只把明确的 `gpu-shadow` fixture 设置为 `cast_shadow=true`；普通无阴影回归数据不变。

既有 fixture 增加只读 `inspect-render` 模式，复用现有 codec 输出 feature 清单，不增加生产 API。新建独立 `E:/lux-ed-d2/shadow-preview`，同时具备七个 feature 与投影点光源；没有覆盖旧 manual-project、用户编辑内容或旧固定 SDK。

正常 RelWithDebInfo Editor 打开新项目后，实际画面出现立方体到地面的阴影。Computer Use 检测到用户操作后停止自动输入，窗口留给用户观察；不记录为自动正常退出，也不把肉眼看到投影当作所有阴影参数/像素正确性的证明。未新增或修改渲染算法、bias、过滤或 GPU 同步策略。

## 验证与边界

复用正常 `work-build/work-sdk/work-consumer`，UI/publication diagnostics 均未启用。相关生产 target 构建、安装与消费者重建成功；`editor.d2.installed_component`、`editor.d2.pane-lifecycle` 两项通过（2.51 秒），包含普通提交/Undo、生成组件、真实 Pane/队列生命周期。没有把旧 44/9 全量成绩换成本轮身份。

原始构建、安装、CTest、旧/新项目 feature 解码结果、截图和产物身份在 `E:/lux-ed-d2/reports/d3-simple-edit-shadow`。本轮没有运行完整阴影像素比较、所有光源/DPI/多 View 组合，也没有重做成本矩阵。

启动新示例：`& "E:\lux-ed-d2\verify\desktop-shadow.ps1"`。它使用正常 work-sdk 和已经建立的 shadow-preview；再次运行不重建或覆盖项目。旧 `desktop-manual.ps1` 仍绑定旧固定候选及旧项目，供历史复核。

仅推送开发分支，不修改或合并 main，不扩大 D3 验收结论。
