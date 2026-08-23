# ADR：Object Model 与 UI vNext 基建冻结

日期：2026-08-23
状态：Accepted（foundation API frozen，consumer migration 暂停）

## 决策

在继续 Engine/Editor 业务重构前，先冻结两层可独立安装的基础原语：

```text
core/meta -> core/object -> function/ui_next -> function/ui_next_vulkan
```

- `core/meta` 只描述类型、静态/实例字段、方法与构造能力；反射不拥有对象。
- `LuxObject` 只拥有对象 affinity、惰性连接状态、weak lifetime 和 targeted event。
- `Signal` 的进程内 identity 是 class-scope static descriptor 的地址；owner、payload
  `TypeToken` 与显式名字只负责类型约束、反射发现和诊断，不建立 Signal Registry。
- `ObjectDispatcher` 是 owner-thread 的不透明消息队列，不是 Executor/EventBus。
- `UISession` 是 ImGui Context、私有 dispatcher、Pane/Factory 注册、focused Context、
  `CommandRouter` 和 Layout 字节的唯一 owner；它不是 `LuxObject`。
- `Pane` 实例由具体工具以 `unique_ptr` 拥有，`UISession` 只持非 owning 注册。
- `Context` 只是交互语境 identity，禁止携带 Project/Scene/Asset/Renderer service。
- `Command` 只做 focused Context 下的语义 dispatch；物理输入仍属于
  `InputContext/ActionMap/ActionMapper`。
- 同一 Pane type 的多个实例可以各自绑定相同 `Command + Context`；
  `UISession` 只让 focused Pane 的 receiver 参与 contextual dispatch，禁止未聚焦
  实例因共享 Context identity 而抢占命令。
- 动态 Object 连接只在 connect-time 查询 `RefStaticField/RefMethod` 并严格接受
  `void(const P&)`；稳态通知不查询 Reflection。Direct/零订阅 notify 不分配。
- `ui_next` 不依赖 legacy UI、Input、Resource、ECS、Runtime、Renderer 或 Editor；
  Renderer/Vulkan 依赖只能出现在 `ui_next_vulkan`。

## 明确不引入

```text
ObjectManager / ObjectTree / EventBus
UIManager / WorkbenchManager / ContextManager / ToolHost
ContributionGraph / keyboard shortcut subsystem
```

## 冻结点

旧 `function/ui` 与 Editor 业务 wiring 本阶段不迁移。只有 foundation 的公共 API、
installed consumer、依赖门禁和测试矩阵全部稳定后，才重新审计 Engine/Editor，制定
consumer migration。不得一边修改 primitive，一边让 Editor 继续长出临时适配框架。

## 验收状态

- `lux-cxx` RelWithDebInfo 49/49，`lux-engine` DEVELOPER 23/23。
- PLAYER 15/15、EDITOR 15/15、TOOLCHAIN 7/7；三个 profile 的第二次全量
  构建均为 `ninja: no work to do`。
- Android PLAYER 与 Android lux-cxx 全目标构建、安装及架构门禁通过。
- Debug 下新增 Object/UI 测试通过；全量 Debug 仍有两个既有 Phase 9
  EnTT 探针在重复插入组件时触发断言，该历史基线问题记录在 unfinished-work，
  不改变本 ADR 的 foundation API 冻结结论。
