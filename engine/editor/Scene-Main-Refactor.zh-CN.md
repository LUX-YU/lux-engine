# Scene / Main / Inspector 重构交付

## 来源与范围

Engine 起点为 `3693b8a185025d26a7e5627597126b7a8285d48e`，开发分支为 `codex/scene-main-refactor`。实际依赖的 lux-cxx 起点为 `3100f54`，本轮通用容器在独立分支 `codex/concurrent-scene-refactor` 实现。不修改 main、并行脚本分支或阴影实验工作树。

仅使用 RelWithDebInfo。不新增运行模式、调度器、全局 Manager/Context、事件总线、动态资源流送或 OOM 工程。本文描述本轮实现，不用旧候选结果为新二进制背书。固定身份、统一资格和原始证据在最终记录中追加。

## 实际职责

```text
Editor::exec / Main
  ├─ 处理输入、Process Main 完成及 LuxObject 通知
  ├─ SceneEditor
  │    ├─ 作者 Scene、选择、资源及作者历史
  │    └─ SceneRun：独立运行 Scene、固定步长、暂停历史、保留的资源
  ├─ ScenePane：一个视口、相机、编辑/运行目标切换
  ├─ Inspector / Outliner：读取当前检查目标
  └─ EditorRenderer：采用 Program、View、渲染线程及 GPU 退休

Process CPU：捕获的编码/解码等有限任务
Simulation：由 Main 发起同步 execute/refresh，TaskExecutor 执行其任务图
RenderSystem：通用稳定点提取脏变化，准备一个待接纳 StateUpdate
Binding：Main 上转交同一个待接纳包，维护渲染 Scene 与关闭事实
Render thread：可靠 Program FIFO、最后采用状态、GPU 生命周期
```

Main 同步单步并不保证任意慢系统都不影响 UI。当前 Run 使用 caller 执行器，重计算应按系统的真实访问契约另行安排，不能宣称本轮实现了异步 Simulation 单步。

## 实施接点

| 议题 | 本轮代码与行为 | 对应验证 |
| --- | --- | --- |
| Run 线程归属 | `SceneRun.cpp` 移除长期 Process worker、互斥/条件变量循环；Main 建立、推进、销毁实际 Scene；Process 只保留有限解码 | fixed-run、dynamic-run 的 Main 身份断言；一个 CPU worker 配置 |
| 不同速推进 | 每轮至多一个模拟步；暂停立即返回；必要发布未接纳时不推进下一步；Stop 不阻塞 Main | dynamic-run、render-thread；零/一 Program 配额 |
| 失败事实 | 保留实际 clock、最后完成的 simulation/stable/publication、失败阶段和首错；Binding 关闭前后均采用持久失败 | simulation-failure、dynamic-terminal、closing-terminal、failure-closing-terminal |
| 作者隔离 | 作者实例保留；运行可变内容独立；Mesh/Material 句柄与 pins 共享；暂停作者派生和绘制 | fixed-run、作者内容/历史/选择恢复 |
| 同视口运行 | 删除 RunPane；ScenePane 关闭旧 View 后才建立新目标 View；同帧图像引用与 GPU 水位保留 | fixed-run 的真实 ScenePane 和同帧图像检查；桌面另记 |
| 检查目标 | Inspector/Outliner 跟随实际运行 World；运行只读、暂停字段可写；运行实体增删在稳定 owner 推进点更新目录 | dynamic-run、fixed-run |
| 暂停历史 | 每次暂停独立 HistoryId；Resume/Step 先结束字段再退役旧历史；旧写请求拒绝；不回退作者 Undo | fixed-run 的编辑/Undo/Redo、派生、旧目标、Step 新历史和 Stop 恢复 |
| 暂停派生 | EVOLUTION 的 refresh 使用同一系统实例的独立派生图；不执行演进/Hook，不推进 clock；派生命令存储不冲掉下一步演进命令 | derivation_protocol 的真实 Transform 和 Hook follow-up 命令 |
| Scene 边界 | 移除 executePresentation、addPresentationTask 和空阶段；world() 更名 worldDescription()，不伪装成运行 World | 全部消费者编译；无 RenderSystem 的 CPU Run |
| 元数据 | RenderSystemMetadata 拥有 Feature 绑定和配置；SceneMetaManager 无 Render 客户端依赖 | 单独 scene-core 安装消费者及依赖检查 |
| 装配 | sceneMetadata 在 editor_scene 业务 target；作者/Run 共用 beginSceneRendering；GUI 只登记 provider 和控件 | 安装生成/链接检查、cpu、fixed-run |
| 目录 | simulation/builtin_systems；scene/builtin_systems/render；移除 scene/presentation target | 新 SDK 无旧 package/header，CMake 和消费者 |
| 通用并发容器 | LatestSpscExchange、BoundedSpscFrameRing 位于 lux-cxx/concurrent；ScriptRuntime 和 RenderProgram 消费者迁移 | cxx scene_exchange_test；可靠 FIFO 与可覆盖观察值语义分开 |
| 同线程发布 | 删除 RenderSyncConsumer 和 Scene→Main 队列；Binding 内一个稳定存储，Pipeline 借用；背压保留原包 | render-thread、terminal-pending/forwarded、动态 Run |
| 预算 | reply 与 Program 配额分离，开文档/已有文档/前端共用本轮余额；文档起点轮换，帧和文档交替先接纳 | Program contention 的 capacity 1/2/3、三种次序，实际预算测试 |
| Inspector | 生成 typed ImGui 写实际组件；首次实际变动捕获 before，结束捕获 after；每次有效变动发布 notifyUpdated | installed_component、transform-sync、容器消费者 |
| 历史接纳 | 已发生字段修改只登记历史，不再次 apply；接纳失败保留 token 和 before/after，重试不再覆盖；未完成记录呈 dirty | 安装消费者的拒绝/重试、Undo/Redo、no-op |
| 保存/切换 | 保存仅捕获作者内容；选中、切换、Play/Step/Resume/Stop、关闭先结束字段；分区只读约束保留 | save 系列、fixed-run、indexed consumer |
| Pane/通知 | Pane 身份按文档 handle 代次，独立于暂停历史；历史接线重绑，旧接收者退役；实际销毁后排队通知不可访问旧 Pane | pane-lifecycle、attach-rollback、局部路由回归 |

## 字段编辑的精确政策

Registry 是交互期间当前值的唯一来源。普通数值不再经过持久 Draft 或 preview.after；小标量的调用前临时值用于在 ImGui 首次写入时捕获 before。字符串仍使用 ImGui 自身输入缓冲，CallbackEdit 在写回字符串之前建立记录。四元数的欧拉表示、页码、容器键输入等局部显示值不作为第二份业务模型。

容器单元素编辑记录元素值；结构插入/删除仍先准备结构结果，不能把它说成完全无复制。结束时读取一次 after，Undo/Redo 通过同一脏通知更新派生结果。非法输入不发布，恢复该手势的 before；正常业务拒绝保留可重试记录。OOM 不增加特殊支持。

用户已取消“持续拖动 Escape 回滚”和“裁剪中的持续手势取消”要求；控件离开、切换或关闭时结束记录，之后使用 Undo。保留完成记录、关闭保留 owner、同帧图像引用与失效身份保护。

## 工程结果记录政策

开发期安装消费者包含 46 个 CTest 注册；普通 Scene 独立依赖消费者和九个 Editor 链接消费者分别统计，不相加为逻辑验收数。诊断消费者的复制计数与正式计时分开。最终构建使用固定干净检出、新 SDK 和迁移前缀，不把工作 SDK 的残留头当作新安装。

开发中曾发现并修正：分区只读保存入口遗漏、测试图像观察器递归、派生测试 Hook 合同错误、调度次序改变后的过早断言，以及旧 RunPane 数量/历史 ID 作为 Pane ID 的测试假定。原始失败日志保留；崩溃或超时均未作为负例通过。

## 尚不应推导的结论

- 两个 World 会增加实体/系统状态；共享资产和单视口不等于零额外内存。
- Main 同步模拟不能保证重系统下帧率恒定。
- 暂停仅开放支持的字段；结构编辑与更换冻结 Mesh/Material 资源集不在本轮。
- 窗口测试、IO 注入、Computer Use 分别记录，历史 IME 成绩不自动延伸到新候选。
- 独立阴影 atlas 布局验证问题保留其原状态，本轮不会因普通 GPU 测试通过而宣布解决。

## 固定候选与最终结果（2026-09-19）

| 用途 | 身份 |
| --- | --- |
| Engine 生产实现 | `4355f7eae02e88e36e252208edf385f80d7ba6c1` |
| 最终代码及测试候选 | `230395b7595f64f6336e1d55e05568603a3f08ff` |
| lux-cxx 通用容器 | `aae62e2fc17ef78ae7be2559a6d58fa0f8297b05` |
| Engine 开发分支 | `codex/scene-main-refactor` |
| lux-cxx 开发分支 | `codex/concurrent-scene-refactor` |

`230395b` 相对生产提交只改变 `run_checks.hpp` 的测试准备次序。Engine 生产目标在该提交上执行 `all -j 4 -- -k 0` 返回 `ninja: no work to do`。本报告的后续提交只记录结果，不改变上述生产身份。

资格使用干净检出 `E:/lux-ed-d2/d3q/s`、既有增量构建树 `d3q/b`、本轮新建 SDK `mainq/sdk` 和迁移位置 `mainq/relocated`。lux-cxx 使用独立干净 clone `mainq/cxx-s` 和新 SDK。更换 cxx 前缀引发的 Engine 988 步编译已经完成；不将此描述为空构建目录的全新编译。最终消费者通过安装包配置，运行 PATH 排除工作 SDK、源码树和旧构建树。

最终注册结果分别为：

| 验证 | 实际结果 | 原始证据 |
| --- | --- | --- |
| Editor 安装测试 | 46/46；50.79 秒 | `complete-ctest.log`、`complete-LastTest.log` |
| fixed-run 重复回归 | 三次独立进程均通过 | `complete-fixed-run-repeat.log` |
| 独立 Editor 链接消费者 | 9/9 | `complete-closure-test.log` |
| 独立通用 Scene 消费者 | 1/1 | `complete-scene-core-test.log` |
| lux-cxx 并发容器测试 | 1/1，包含并发交接、最新值和可靠 FIFO 语义 | `q-cxx-test.log` |
| 隔离的组件复制计数消费者 | 1/1 | `complete-copy-test.log`、`complete-copy-LastTest.log` |
| 生成器负例 | 准确拒绝缺失的反射组件，全部既存输出字节不变 | `generation-negative.log`、`generation-preserved.json` |
| 实际依赖与链接 | 通用 Scene 的 include/DLL 闭包无 Render、Editor、ImGui、GLFW；生成 Inspector 对象链接 editor_scene_ui | `scene-core-include-deps.txt`、`dll-closure.json`、`generated-link-inputs.txt` |
| 二次构建 | Engine 和安装消费者均为 no-op | `complete-engine-noop.log`、`complete-consumer-noop.log` |

上述单位不相加为“完整产品验收用例数”。46 项中保留了实际 Process、保存/重试、结构回放、Pane 退役、动态 Run、CPU Run、派生图、Transform 同步、真实 Render 终止和 JR 首错保护等相关路径。安装测试显式启用断言。复制计数只编入单独测试组件，正常 SDK 与成本消费者不携带这项计数或新增 OOM 注入。

通用 Scene 与 Editor 的 Scene 业务包应区别：后者仍显式依赖 `editor_rendering`，所以本轮并未交付一个完全无图形依赖的 Editor 产品。RenderSystem 可选的业务运行和通用 Scene 的无渲染依赖已经分别验证。

## 正式窗口结果

使用 `mainq/relocated/bin/lux_editor.exe`、独立项目 `mainq/desktop-project-final`、系统字体显式冷配置。没有分发字体文件。输入来自 Computer Use 经 Windows 的鼠标/键盘路径，未以 Pane 方法注入冒充桌面操作。

| 操作 | 最终候选实际观察 |
| --- | --- |
| 作者 Object 1 的 Translation X | 0 → 1.5，生成 Inspector 与场景位置同步变化 |
| Play | 原 Scene 视口切换为运行世界，没有另建 RunPane；Outliner 显示 Run objects，结构编辑入口禁用 |
| Pause | 停在 580 步；选择运行对象，X 从 1.5 → 3，画面更新，步号仍为 580 |
| 暂停 Undo / Redo | X 恢复 1.5 / 3，画面对应变化，作者未保存状态保留 |
| Step | 580 → 581，只推进一个固定步长 |
| 新暂停区间 Undo | X 保持 3，没有回退到上一暂停区间或作者历史 |
| Resume | 时钟继续；Inspector 只读，拖动字段未改变 X=3 |
| Stop | 作者选择与 X=1.5 恢复；随后 Ctrl+Z 将作者 X 恢复为 0 |
| 关闭 | 明确点击 native X；日志记录 native-close、review-begin、review-commit、finished；实际进程退出码 0 |

`final-desktop-*.jpg`、`final-desktop-trace.json`、`desktop-final-process.json`、`desktop-final.stderr.log` 和 `desktop-final-exit-code.txt` 保存本次事实。前一轮桌面检查的截图与日志单独保留，不覆盖为最终候选结果。

这条桌面路径不代表所有 DPI、IME、窗口组合和 FAILED View 的人工恢复都已验证。历史未知 native-close 来源没有被这次指定关闭解释。用户已取消的拖动 Escape 回滚需求不再作为待通过门槛。

## 有限成本

每种情形五个独立正常进程；完整样本在 `cost-summary.json` 和 `cost-1` 至 `cost-5` 原始日志。以下为最终候选五样本中位数，无旧候选等量对比，故不报告加速比例。

| 情形 | 实际完成量 | 主动成本或阶段延迟中位数 |
| --- | --- | --- |
| UI，256 个作者对象 | 20 帧 warmup，100 个测量帧，4 个渲染对象，1600×900，同一可见范围 | 累计主动绘制 10.595 ms；主动 packet retry 0.045 ms |
| UI，4096 个作者对象 | 同上，作者目录扩大 | 累计主动绘制 11.071 ms；主动 packet retry 0.046 ms |
| 256 元素容器单元素编辑 | 连续 100 次更新，1 条历史，完成后结构恢复及回放 | 主动 24.2 μs；历史计费 422 字节 |
| 4096 元素容器单元素编辑 | 同上 | 主动 22.2 μs；历史计费 422 字节 |
| 动态 Run | 6 个模拟步、6 个发布包，主动停止消费制造背压 | 模拟主动累计 0.3427 ms，必要发布等待累计 17.4171 ms |
| Run 控制 | 同一动态场景 | Pause 2.041 ms；Step 5.228 ms；Stop 至系统退出 1.299 ms，至 Main 最终结果 1.329 ms，至资源退出 89.808 ms |
| Resize/retry | 2 个 View、9 次 resize 请求，旧 256×128，新 320×192 | packet 重试 1.9759 ms；旧 lease 保留阶段 31.6115 ms；释放至 READY 6.352 ms；READY 至关闭 11.1624 ms |

控制和退休延迟包含驱动轮询与资源等待；发布等待包含人为停止消费，不能当作正常稳态卡顿。100 帧累计绘制不是完整帧率，历史计费不是进程内存或 GPU 内存总上界。对象规模样本保持渲染对象数固定，只检查目录规模成本。

独立复制诊断在两种容器规模下均记录一次结束时的 after 捕获。保留的旧测试输出使用 `adopted preview copies` 字样，该计数实际覆盖当前完成字段记录的路径，并不表示仍存在通用 preview/Draft 数据副本。诊断耗时不混入上述正常成本。

## 失败记录与修正边界

原始失败没有删除或重标通过：

1. 初次新 cxx SDK 配置漏开 reflection generator，配置失败后未运行旧 EXE；修正构建选项后重建。原记录在 `qualification-attempt-1`。
2. Resize 成本第二次进程遇到队列已被前端填满，测试断言“本轮一定先新接纳至少一帧”不成立。改为核对实际保留包和旧图像 lease；下一阶段仍必须成功提交原包，并通过真实 GPU_COMPLETE 与关闭保护。生产协议未因此放宽。原记录在 `cost-attempt-1`。
3. fixed-run 测试曾将“资产就绪”当作“作者 StateUpdate 已采用”。Run 会暂停作者派生，因此测试现在先查询并确认作者三个 Mesh 已采用，再 Play 检查作者/运行隔离。原 `release-ctest.log` 保留准确失败，之后三次定向及最终统一回归通过。
4. 桌面暴露 Outliner 的运行标签和结构操作入口遗漏，已在 `4355f7e` 修正；实际写入口本来已经拒绝运行中的结构操作。

## 交付位置与运行

实际源码与原始证据归档：`E:/lux-ed-d2/deliveries/scene-main-refactor-230395b/source-and-reports.zip`。其中包含两个固定提交的 Git 源码 ZIP、实施报告、构建/测试/成本日志、桌面截图、manifest、源码与产物身份。Git blob SHA1 与 Windows checkout SHA256 分开记录，不能直接比较成“源码不一致”。

可运行本轮独立项目：

```powershell
$env:PATH = 'E:/lux-ed-d2/mainq/relocated/bin;E:/lux-ed-d2/mainq/cxx-sdk/bin;E:/SyncForder/CodeRepos/install/o/v4/lua55/bin;E:/SyncForder/CodeRepos/install/RelWithDebInfo/bin;D:/Development/vcpkg/installed/x64-windows/bin;' + $env:PATH
& 'E:/lux-ed-d2/mainq/relocated/bin/lux_editor.exe' --project 'E:/lux-ed-d2/mainq/desktop-project-final/Project.luxproject' --font 'C:/Windows/Fonts/msyh.ttc'
```

本轮结论为 Scene/Main/Inspector 重构候选完成并提交独立审阅；不声明整个 D3 产品、所有设备/输入组合、任意重负载帧率或独立阴影问题已经通过。按用户既定授权推送两个开发分支，不合并 main、不发布、不冻结。
