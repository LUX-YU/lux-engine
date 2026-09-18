# Inspector 数值变化而场景延迟更新：定向调查与修正

日期：2026-09-18。分支：`codex/editor-d3-foundation`。

基线：`1ba2d249dfa53ecc18b59505f674c825da55ffc2`。
代码／测试候选：`c56aef1ab5baa7788ea494a9dec3d9ab975a7284`。
未修改 main；本次没有重新设计 Scene、Run 或 Renderer。

## 结论与修改

用户描述为 Object 1 的 Translation X 数值改变，松手后物体仍不动，稍后突然变化。
发现一条能产生这种延迟的具体路径：Editor 每轮先调用 frontend.poll，再调用
pollDocuments。GuiFrontend 内的 Renderer.poll 会优先向共用 Program ring 提交排队的
UI Frame；SceneRenderBinding 随后才重试 StateUpdate。GPU 较慢、队列持续拥挤时，
Frame 可以反复占用刚释放的位置，场景更新持续背压。UI 控件已经读取新的作者值，
渲染后端却继续使用此前采用的场景内容。

生产修改仅调整 Editor 主循环顺序：文档先派生并重试已有更新，再推进前端渲染。
Main 完成投递仍在两者之前。输入采集、文档轮转、有限队列、输入保留、GPU 退休、
Run 的保守背压政策均保留。不增加队列、调度器或逐帧完整场景复制。

这消除了已确认的接纳顺序缺陷；不声称排除了所有可能导致画面延迟的原因。

## 实际证据

1. 沿生成控件、作者 Transform3D、WorldTransform3D、提取阶段及后端 TransformBatch
   核查。临时 trace 看到后端对存活 instance 应用正确的 X 值；该 trace 已全部移除，
   对应 DLL 已重新构建并安装。没有提交 modules 或 Scene Render 的生产改动。
2. 使用真实 RenderProgramSession 和真实有界 ring，固定消费者每轮处理 StateUpdate
   直到一个 Frame，复演争用。此项是受控协议实验，不是 GPU 耗时或物理桌面复现。
   每组 256 轮；旧顺序在容量 1／2／3 下均消费 256 个 Frame、接纳 0 个更新。
   新顺序均接纳并按序消费 64 个更新，分别消费 192／234／255 个 Frame。
   拒绝时原更新字节保留；没有重复、跳序或假计数。
3. 真实 Editor.exec 回归在绘制阶段修改真实 Scene，下轮前端推进时核对世界坐标。
   修前首轮明确得到 author X=0.05、World X=0，随后正常清理 owner，再由结果断言
   报失败；没有把超时或任意崩溃当作负例。该检查证明推进顺序，不单独证明长时间卡住。
   修后 120 次连续修改通过。
4. 实际安装的 generated Transform UI 接收 UISession 输入注入：三个对象各 XYZ 拖动，
   共九次，检查实时世界坐标、松手只提交一条历史、Undo 恢复。属于 UI 事件注入，
   不标为物理鼠标测试；没有恢复用户已取消的拖动中 Escape／裁剪取消要求。
5. 正常 RelWithDebInfo SDK 的 17 项相关 CTest 全部通过，耗时 39.87 秒：
   installed_component、save、material-gui、flow-gui、pane-lifecycle、fixed-run、
   fixed-run-failure、render-association、render-thread、transform-sync、dynamic-run、
   simulation-failure、dynamic-terminal、closing-terminal、failure-closing-terminal，
   以及 terminal-pending／terminal-forwarded。测试格式整理后 transform-sync 再通过一次。
   这些是 17 个测试注册，不等于 17 项完整产品验收。
6. 修后正式 lux_editor.exe、项目副本、Windows Computer Use：Object 1 的 X 从 0
   拖到 3，截图确认红色物体和阴影移动。随后反向拖动时也观察到物体移动，但下一次
   抓图时窗口已消失。日志只有 native-close-flag／review-begin，缺少最终关闭证据。
   该过程不记为完整桌面闭环，也不根据进程 exit=0 推断用户主动正常关闭。

## 限制与独立发现

- 本次为工作构建及安装 SDK 的定向验证，未重跑完整 clean-clone／消费者全集资格，
  未测量性能提升百分比。上表不能外推到任意 GPU 负载和所有文档组合。
- 阴影 fixture 在 Vulkan validation 开启时另有真实错误：shadow atlas 的数组层
  1／2／3 仍为 UNDEFINED，而绘制期要求 DEPTH_STENCIL_READ_ONLY_OPTIMAL，
  `VUID-vkCmdDraw-None-09600`。相关初次调查程序因此失败，原日志保留；
  不计为通过，也未证明此错误与本次更新饥饿同源。本轮未顺带修改阴影资源布局。
- 正式桌面拖动使用带阴影项目副本；通过的自动 GPU 回归使用各自既定非阴影 fixture。
  不以这些通过结果替代阴影 validation 验收。
- 测试过程中几次窗口退出的发起来源仍未确认，没有归咎于用户或工具。

## 产物与复核

源码可从上述固定开发分支提交取得。正常 SDK 为 `E:/lux-ed-d2/work-sdk`，
原有 `E:/lux-ed-d2/verify/desktop-shadow.ps1` 启动脚本指向该 SDK；须重启旧进程才会
采用新的 Editor Core DLL。未改写用户原项目内容。

原始证据目录：`E:/lux-ed-d2/reports/d3-transform-drag`。
重点文件：`order-before-run.log`、`order-after-run.log`、`contention-after-run.log`、
`affected-ctest.log`、`final-transform-ctest.log`、`fixed-x0.png`、`fixed-x3.png`，
以及保留失败的 `before-transform-sync.log` 和各 `*desktop*stderr.log`。
`manifest.json` 分别记录 Git blob、Windows checkout SHA256 和安装二进制 SHA256。
本地证据压缩包为 `E:/lux-ed-d2/reports/transform-update-evidence.zip`；不包含依赖或字体。
