# ADR：Object Model 与 UI vNext 基建稳定化

日期：2026-08-23
状态：Accepted（foundation API frozen，consumer migration 仍需重新审计）

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

## 冻结边界

旧 `function/ui` 与 Editor 业务 wiring 本轮未迁移。Foundation API 现已冻结；
后续仅允许在重新审计 Engine/Editor consumer migration 后开始上层接入，不得
自动恢复任何旧迁移方案。

## 冻结验收（2026-08-23）

- `lux-cxx` RelWithDebInfo 与 Debug 均为 49/49；`lux-engine` RelWithDebInfo
  为 27/27。Object/UI 的 Debug 测试全部通过。
- Object 与 UI 安装后 consumer 分别通过独立 codegen、link 与 run；
  cross-DLL Base/Derived Signal prefix 布局已由真实 DLL 覆盖。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 与 Android PLAYER 全量构建通过；
  各 CMake 树第二轮均为 `ninja: no work to do`。
- Android NDK Clang 19 完成全量 PLAYER 编译，产物包含纯 `ui_next` 和
  `ui_next_drawdata`，不包含 GLFW 或 ImGui Vulkan backend。当前无目标设备，
  因此未宣称已执行 Android ASan/UBSan 运行时测试。
- Direct/Queued、churn、reentrancy、Pane/Command/Context 性能基线已记录，
  Direct notify 的 consumer-path 分配计数为零。
- 两个既有 Phase 9 EnTT Debug probe 仍单独记账；它们不属于
  Object/UI Foundation，不改变本 ADR 的冻结结论。
