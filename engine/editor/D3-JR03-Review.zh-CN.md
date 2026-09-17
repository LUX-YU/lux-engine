# D3 JR-03 与桌面收尾记录

本轮保留既有资源 owner、值解码、Main/worker、固定步长 Run 与 Editor 拓扑。没有新增运行模式、流送、时基、调度器或 OOM 工程。

## 源码与证据身份

- 起点：`999aa1d0f46b8f706e65496afcf46ea6146c53b5`，工作区 `E:/lux-ed-d2/src`，分支 `codex/editor-d3-foundation`；开始时干净，没有额外适用的 AGENTS。
- 只增加关闭观察值及负例的修前检查点：`23eec263`。它基于上一候选加入只读 drain 接纳观察，不改原错误传递控制流；不是未经修改的旧 SDK。
- JR-03 生产修正：`74a4adee`。测试驱动顺序修正：`406146290977db4694668b55aa5a674a910dbd8d`。
- 桌面发现的同帧隐藏问题修正：`81225f5ef5272166b87b5f7e05e8112974086004`。
- 修饰键与 Run 视口布局修正、最终代码／测试候选：`3c82ad1587f38193f73eb52d2428f6b63b9441a6`。
- 报告提交与代码候选分开。最终归档提供完整 tracked source、Git blob OID/SHA256、Windows checkout SHA256、SDK/消费者产物哈希、原始日志及截图。

审阅方提出 JR-03 时是静态源码判断。本报告中的修前、修后执行来自本轮实施方本机运行，不能归为审阅方复跑。

## JR-03：先采用失败事实，再决定关闭结果

`SceneRenderBinding::hasFailure()` 直接返回既有 `failure_recorded`，与 CLOSING/CLOSED 生命周期状态独立。`SceneRun::poll()` 在两处 Binding 推进之后使用同一个观察过程：读取运输统计及 drain 接纳事实，采用首个原始失败，再检查 CLOSED、释放 Binding 和 pins，最终决定 FINISHED/FAILED。

关闭途中的渲染错误记为 PUBLICATION 阶段，不因 worker 已结束而误记 STARTUP。已有 Simulation 错误不被之后的 ChannelStopping 覆盖。没有通过默认错误枚举猜测是否失败，也没有改动 GPU/后端退休条件。

只读 `drainSubmitted()` 与 `RunStatus::render_drain_submitted` 表示正常关闭标记被 Program 客户端接纳，**不表示服务端采用或 GPU 完成**。回归利用它证明先后次序，没有把普通 STOPPING 当成 drain 已开始。

| 实际执行 | drain 接纳时 | 终局结果 | 资源 |
| --- | --- | --- | --- |
| 修前 `closing-terminal` | worker 结果已收到，View 已关闭，Run 成功，published=forwarded=6，pins=3 | 错误地 FINISHED/success | pins=0、pending=0，最后 runtime leases=0；清理完成后驱动明确 exit=2 |
| 修后 `closing-terminal` | 同一先后次序；随后既有 channel 发布 ChannelStopping | FAILED，`run.render`，原 RenderError 的 type/args 保留 | worker/View/Binding/pins/leases 全部退出 |
| `failure-closing-terminal` | 已有 system=3 的 Simulation 失败，随后进入正常 drain，再终止后端 | FAILED，仍为 `run.simulation`、SYSTEM_TASK_FAILURE/system=3 | 资源全部退出 |

正常 Stop、先终止再关闭、stable/Simulation 失败进度、动态 Run 背压与作者隔离回归继续保留。没有将真实 GPU 测试扩大解释为物理设备丢失测试。

## 桌面实际发现：关闭按钮与当前帧图像引用

在 `4061462` 的正式安装窗口中，通过原生桌面输入依次 Play、Pause、Step、Resume，然后点击活动 Run 面板的关闭按钮。窗口变灰，日志持续为 `editor.renderer:10`，即 INCOMPLETE_FRAME_REFERENCES。

原因是 Pane 在本帧 draw 中变为不可见，但该帧仍记录了它的图像；随后 `appendFrameImages()` 以新的 `visible()` 过滤引用，导致封印拒绝。失败的 snapshot 被保留重试，旧引用已经无法补回。

修正限于原 owner：ScenePane、RunPane 收集已经绘制的 image lease，直到现有 seal/releaseFrameImages 完成，不再用“现在可见”判断“本帧是否绘制”。没有丢弃 Renderer 的完整引用校验，也没有提前释放 CPU/GPU owner。Run 的空闲、正常结束和失败提示同时明确区分，结束后 Stop 不再可用。

原 Run 集成测试中的 `RunImageProbe` 增加 Scene 与 Run 两种真实 Pane 的同帧隐藏引用检查。它是接口级相邻回归；正式窗口点击及画面记录另列，不能互换。

在 `81225f5` 的正式窗口中，隐藏活动 Run 后主窗口继续可用；恢复 Run 后却观察到图像在就绪、黑色与未就绪提示之间切换。提示行只在 NOT_READY 时占用高度，使后续 viewport 的尺寸随 readiness 改变，又触发下一次 resize。最终候选为就绪和未就绪保留相同提示行高度，不改变 resize、GPU 退役或 NOT_READY 的含义。

## 桌面实际发现：文本快捷键的修饰键遗漏

在 `81225f5` 上，真实 Windows IME 的 `n → i → Space 选词` 显示“你”，保存后 Material 源文件包含 `U+4F60`、UTF-8 `e4bda0`，不是粘贴或直接 UiText 注入。原始截图、动作顺序、保存源及字符证据位于 `desktop-final`。但同一输入中的 Ctrl+A 和 Ctrl+Z 无效；场景中已提交的 X=4.5 未被撤销。

进一步通过既有 `UISession::feedInput` 复现：LeftCtrl 的物理键已经 down，ImGui 聚合 Ctrl 却为 false。修前驱动在明确该事实后正常清理并返回 2，没有把断言崩溃当成负例。

修正在现有 UISession 输入接点同步 Ctrl/Shift/Alt 聚合事件，分别保留左右键状态；释放一侧时不清掉另一侧，失焦清空输入侧记录。没有增加输入系统或公开类型。现有 foundation consumer 增加实际 InputText 的 Ctrl+Z、左右键组合、失焦和重获焦点检查，修后通过。桌面复查与事件注入分别记录。

## 资源关联测试的驱动等待问题

本轮受影响测试中，旧 `render-association` 在 stage=91 超时；上一轮原 SDK 在当前环境也发生同类失败。进一步记录显示 phase=11、pending=1，Renderer READY，frames=4935、GPU completed=4933、render events=0、validation errors=0。

驱动总在 GUI 提交之后争用 Program 槽，可能长期失去接纳机会。将测试操作放到正常文档所在的“先业务、后呈现”顺序后，连续三次通过，固定候选中也继续验证。只改测试调用顺序与失败诊断，不新增生产调度或隐藏失败。原超时和断言日志保留，不计作准确产品负例，也不将它们重标为正常退出。

## 资格与桌面范围

最终候选 `3c82ad15` 使用独立干净检出 `d3q/s`、既有增量构建树 `d3q/b`，安装到新前缀 `d3jr03q/ready/sdk`，复制到 `ready/relocated`，从安装包重新配置并编译消费者。生产和消费者的第二次 all 构建均为 `ninja: no work to do.`。运行时不从开发构建树或旧 SDK 搜索 DLL。

- 44 项安装 CTest 全通过，100.12 秒；包含正常 Stop、两个 JR-03 先后次序、此前终止/失败进度/动态背压、资源关联及既有保存/发布路径。
- 9 个独立模块消费者全通过；include/link/DLL 闭包检查通过。
- 生成器缺失组件准确拒绝，先前生成文件保持原字节。
- 同一候选完成五次独立进程的动态 Run 有限成本记录。

以上是不同统计单位，不相加为“53 项完整验收”。完整原始结果位于 `q-ctest.log`、`q-LastTest.log`、`q-closure*.log`、`q-generation.log`、`cost-*-dynamic-run.log`；逻辑映射见 `acceptance-results.csv`。

闭包边界保持原有事实：Core/Material/Flow 业务消费者不含 UI/window DLL；Scene 仍经 editor_rendering 引入 ImGui/GLFW，因此本轮不宣称 Scene 已成为纯 CPU SDK。正常构建的诊断选项为 OFF，不拿它替代专用故障诊断资格。

### 正式窗口的实际结果

桌面输入使用 Computer Use 经 Windows 输入路径作用于正式安装 EXE；不是调用 Pane/Document 方法的接口注入，也不是人手硬件操作。新候选截图和动作在 `desktop-ready`，修前/中间候选分别留在 `desktop`（4061462）与 `desktop-final`（81225f5），不能因为目录名含 final 就把它当成最新二进制证据。

| 工作流 | 新候选实际观察 | 边界 |
| --- | --- | --- |
| Play/Pause/Step/Resume/Stop | Pause 稳定在 357；单次 Step 到 358；Resume 继续；Stop 后显示 Run finished | 静态作者场景桌面操作；动态系统组合另由 GPU 驱动测试证明 |
| 活动 Run 隐藏/恢复 | 图像已经绘制时点 X，主窗口继续；Restore 后图像正常，后续 step=832、1288 两次观察保持图像 | 证明本次路径；不声称长期像素连续性或完整 Pane 对象销毁重建 |
| 局部文本 Undo/Redo | Scene 先产生 X=3→4.5 的历史；Material 名称 Ctrl+A，真实 IME 输入“你”；两次 Ctrl+Z 撤销插入和选区删除，恢复 Saved S1；两次 Ctrl+Y 恢复“你”；Scene 始终 X=4.5/Unsaved | ImGui 将选区删除和 IME 字符插入记录为两次文本操作，不伪称一次内容 Undo；没有调用业务 Undo 替代按键 |
| 真实 IME | n→i 候选窗→Space 选“你”，画面显示；保存源 name 为“你”，U+4F60，UTF-8 e4bda0 | Material Name 路径通过；不扩成所有 IME、DPI、多窗口组合通过；不分发字体 |
| 退出取消及保存退出 | native X→review 1→Cancel→重新打开 Logic 成功；再次 native X→review 2→Save→commit→finished code=0/failure=0/joined=1 | 操作与日志对应，不以 exit0 单独推断用户关闭；日志中的 document.close.save:0 原样保留 |

原始保存文件、Unicode 值/哈希、退出日志均随包交付。没有强制结束这些桌面进程。

### G1—G6 与未完成范围

| 门槛 | 状态 | 证据与限制 |
| --- | --- | --- |
| G1 源码归属/内聚 | 本轮保持 | 15 个代码/测试文件的窄修；没有新增 Manager、Context、调度器或输入框架 |
| G2 定向正确性 | 本轮通过 | JR-03 修前准确失败/修后通过，首错不覆盖；同帧图像引用与修饰键相邻回归 |
| G3 实际用户闭环 | 部分通过 | 上表桌面工作流；持续按住鼠标期间的取消、裁剪中物理手势、完整 C19 Pane 销毁/通知组合仍未取得新的桌面结果 |
| G4 异步/发布 | 相关回归通过 | 既有 Process、保存重试、部分发布、崩溃恢复及冲突测试；不扩展磁盘或业务能力 |
| G5 工程资格 | 本候选通过 | 44 CTest、9 独立消费者、生成与闭包、新安装迁移、all/no-op、源码/产物身份 |
| G6 代表成本 | 有限 Run 数据已取得 | 五组同完成量；不代表完整编辑器性能、大规模结构编辑或逐操作尾延迟 |

RunPane 的明确 FAILED View 提示和 Reopen 的真实桌面错误链仍未运行，正常隐藏/恢复不能替代。当前工具只有完整原子 drag，未提供可确认的“鼠标持续按下时再发 Escape”控制，所以不把拖动后发 Escape 记成物理取消。接口注入的 Esc、裁剪、零布局/折叠与 Pane 重建测试保留原等级。历史非预期 native-close 的来源仍未查明，本次明确操作不能替它结案。

### 有限成本

五组均为 7 步、发布 6 包、必要发布背压 1 次，validation 关闭，驱动主动暂停消费约 140 ms。中位数：Pause 到稳定 1.240 ms；Step 到完成 1.080 ms；Stop 到系统退出 1.206 ms；Stop 到 Main 收到完整结果 3.325 ms；Stop 到资源完全退出 89.139 ms；主动模拟工作累计 0.2566 ms；发布等待累计 129.9148 ms。

输入、窗口/View 数和节奏沿用已有 dynamic-cost 驱动，完整五组数值随 CSV/原始日志交付。控制延迟包含约 1 ms 驱动轮询；发布等待主要由主动停止消费产生。资源退出包括 View 与 Program drain，本轮没有新增二者独立计时点，不将总量解释为 worker 唤醒成本。没有据此声称 FPS、p95、结构操作加速或相对旧候选的性能提升。

`pre-ui-4061462`、`pre-input-81225f5` 分别保存前两次资格的 44 项安装测试、9 个独立消费者和成本记录，不用于给 `3c82ad15` 二进制背书。各次资格均使用独立干净检出及增量构建树、新安装/迁移前缀，不能称为每次从空构建树全量重建。

已知驱动错误单独保留：安装工具中 Windows rc.exe 路径反斜杠被 CMake 当作转义，修正为标准路径后重新配置；资格脚本向后续构建泄漏精简运行 PATH，恢复构建环境后完成；一次局部 consumer 编译未加载 VS 环境而缺失 cassert，修正后才执行新 EXE；桌面证据记录函数两次引用了旧 REPL 变量，输入后重新观察，没有盲目重发输入。它们不是 Engine 缺陷。

本轮通过明确 Alt+F4 结束灰屏窗口，日志包括 native-close、review-begin、review-commit、finished code=0/failure=0/joined=1。只证明本次明确动作的来源，不能推断此前未查明的 native-close 来源。

正常 SDK 的 UI/publication diagnostics 保持 OFF。原诊断资格、旧结构操作成本及此前有效证据继续保留原身份。没有合并或修改 main，没有发布或冻结。完成后推送开发分支，交独立审阅；不声明 D3 完整产品验收通过。

实际交审包名：`LUX_D3_JR03_3c82ad15_source-and-reports.zip`，附同名 SHA256 文件。包内 `manifest.json` 校验所有源码/报告/原始证据条目；`source-identities.json` 分列 Git blob OID、blob SHA256 与 Windows checkout SHA256；`installation-manifest.json` 校验原安装与迁移副本一致；`artifact-identities.json` 绑定 DLL/EXE 与代码候选。验证工具也随包提供，没有将本地摘要代替归档。
