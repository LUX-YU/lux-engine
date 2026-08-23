# ADR：Object Model 与 UI vNext 基建稳定化

日期：2026-08-23
状态：Accepted（foundation stabilizing，consumer migration 暂停）

## 决策

在继续 Engine/Editor 业务重构前，先稳定两层可独立安装的基础原语：

```text
core/meta -> core/object -> function/ui_next -> function/ui_next_drawdata
```

- `core/meta` 只描述类型、静态/实例字段、方法与构造能力；反射不拥有对象。
- `LuxObject` 只拥有对象 affinity、惰性连接状态、weak lifetime 和 targeted event。
- `Signal` 的唯一 authored identity 是 declaring C++ static member；生成的
  `SignalIndex` 只是 inheritance-lineage 内 build-local 的 dense execution coordinate。
- `ObjectDispatcherRef` 是不可变的 affinity/message capability；它只接受
  `ObjectMessage`，不是 Executor/EventBus，也不公开 arbitrary task surface。
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
  `ui_next_drawdata` 只复制 ImGui draw data，不拥有 Renderer/Vulkan integration。

## 稳定化合同

- `LuxObject` affinity 在构造时固定；connection topology 的物理变更只发生在
  sender affinity。
- receiver 析构立即阻止未来 callback；跨 affinity disconnect 先逻辑取消，再由
  sender 侧完成物理清理。
- Direct notify 栈上同步销毁 sender 不受支持；owner 必须在 safe point 延迟销毁。
- typed Signal 使用 reference NTTP 与生成 thunk；Reflection 只参与 dynamic connect-time。
- queued Signal、posted Event 与 connection control 共享 `ObjectMessage` transport，
  但 Signal/Event 的 public semantics 保持不同。
- `UISession` 是唯一 UI session owner；Context 只表示交互语境。
- Command activation scope 与 handler receiver 是两个独立 weak lifetime。
- Published tuning 不锁定 `SignalIndex` 宽度、container inline capacity、SBO 大小或
  atomic 具体表示。

## 明确不引入

```text
ObjectManager / ObjectTree / EventBus
UIManager / WorkbenchManager / ContextManager / ToolHost
ContributionGraph / keyboard shortcut subsystem
```

## 稳定化边界

旧 `function/ui` 与 Editor 业务 wiring 本阶段不迁移。只有 Object、Reflection、UI、
cross-affinity、cross-DLL、installed consumer 和构建矩阵全部稳定后，才重新把状态
改为 `foundation API frozen`，并重新审计 Engine/Editor consumer migration。

## 稳定化起点

- 先前 PoC 基线为 `lux-cxx` RelWithDebInfo 49/49、`lux-engine` DEVELOPER 23/23；
  它证明方向可行，但不再等同于 API frozen。
- PLAYER 15/15、EDITOR 15/15、TOOLCHAIN 7/7；三个 profile 的第二次全量
  构建均为 `ninja: no work to do`。
- Android PLAYER 与 Android lux-cxx 全目标构建、安装及架构门禁通过。
- Debug 下新增 Object/UI 测试通过；全量 Debug 仍有两个既有 Phase 9
  EnTT 探针在重复插入组件时触发断言，该历史基线问题记录在 unfinished-work，
  不改变本 ADR 的 foundation API 冻结结论。
