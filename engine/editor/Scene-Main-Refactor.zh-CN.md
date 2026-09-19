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
