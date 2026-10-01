# P11–P13 持续约束与交接

本章用于更新后续实施文档，不授权当前LLM提前进入后续阶段。旧阶段编号、原功能义务和既有故障责任保持，物理路径按新五层解释。

## 1. P11：命令与扩展

| P11工作 | 唯一位置 | 必须遵守 |
|---|---|---|
| Command查询/Invocation/DispatchReceipt | activities/commands | 固定目标与注册版本，不依赖当前焦点补目标 |
| 会话生命期契约 | editing | 不把所有领域功能塞IEditSession |
| 作者格式/codec贡献 | 对应authoring或原schema | 静态约束不替代运行时code寿命 |
| 具体保存/编译角色 | activities/对应域 | 真正格式/采用职责，不套多层Adapter |
| ViewFactory/控件贡献 | workbench对应域/desktop契约 | 只构造完整DetachedView，不私自open内容或直接挂Root |
| plugin加载与贡献安装 | application/extensions | 调原engine loader，不自建平台装载器 |

完整P11的命令代码寿命、注册撤销、批次快照和动态工厂仍需实际实现/验证；本施工不会以一个concept demo代替。C03责任仍在P11。

concept用于真实静态实现的约束，未知插件在真实异构边界动态绑定一次。普通fixed组合可直接类，不要给每个动作配I/Provider/Port/Adapter。

## 2. P12：完整产品和旧框架删除

- 不需要UI的SaveAll、关闭内容决定、重载和恢复进度归activities。
- `OpenAndShow/ExitEditor/RestoreWorkbench` 等跨内容与Host/平台的组合归application，持有组合进度而非复制底层状态机。
- ViewHost执行挂载和布局，WorkspaceStore只文件，authoring/layout只纯计划；禁止E2 include E3来图省事。
- 唯一产品入口改用正式新链，旧EditorContext、PaneManager、旧三大Editor、到期transition及旧注册入口同阶段删除。
- 不把旧target改INTERFACE链接全部新库保留旧名字；不创建永久legacy；不通过Old/New option长期双轨。
- 旧文件格式和用户数据只读迁移保留，数据兼容与旧业务框架是两件事。
- C01完整旧布局应用、C04启动失败责任在P12按原合同结清。

ProjectBuilder.cpp用户差异继续保护到它被用户授权整合；旧工作区存有diff不等于可丢弃。

## 3. P13：验证与最终收敛

P13不为未删除的旧框架新增延期。实际用户声明支持的环境才报告支持；当前Linux无环境仍不能声称支持，但也不恢复成每轮开发硬阻塞。获得环境或要正式承诺Linux时再建立真实资格。

真实系统IME、完整产品退出、安装、DSO边界、GPU退休/像素/输入和持续运行按适用范围验证。不重复已经被用户停止的旧算法长测，仅测需要决策的新回退或变化。

## 4. 后续代码审阅的七个问题

1. 新文件属于哪一层、哪个领域？是否有新的混合顶层？
2. 是否直接使用原LuxObject、Process、SceneRuntime、RenderRuntime、toolchain、lux-cxx？
3. 新类型承担什么独立事实/生命周期？没有就用函数或合并。
4. 此抽象为何静态/动态？concept有真实算法和实例化点吗？
5. 源码/API/模板/link/装载依赖是否一致？
6. 旧入口是否真实删除，或有被授权的有限consumer/期限？
7. 行为与性能证据是否支持结论，未测项有没有被夸大？

出现新core/services/runtime/adapters等顶层需要重新说明，不允许以功能多了为由恢复旧混合组织。
