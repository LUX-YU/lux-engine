# ADR：Object Model 与 UI vNext 基建稳定化

日期：2026-08-23
状态：Accepted（ontology frozen；public API stabilizing；implementation stabilized
pending independent audit；consumer migration frozen）

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

## 当前稳定化边界

旧 `function/ui` 与 Editor 业务 wiring 本轮未迁移。Object/UI 的 ontology 已经
足够稳定，但 public API 与 implementation 重新进入 correctness/performance
stabilizing。上层 consumer 继续冻结；Engine/Editor consumer migration 仍需
另行重新审计，不得自动恢复任何旧迁移方案。

本轮额外锁定：typed Signal callback 是 `noexcept` notification；typed hot path
由 `Object<Derived, Base>` 在编译期证明 Signal owner；`ObjectWeakRef` 的跨线程
能力只有 liveness 与投递；UI route 只在 focus/context/binding 事实变化时重建。

## 已失效的冻结验收记录（2026-08-23）

以下记录保留为历史事实，但不再支持 `foundation API frozen` 结论。
Direct Event affinity、UISession 结构重入、Pane non-owning lifetime 和
CommandRouter callback reentrancy 均尚需重新稳定。原性能报告也没有在
仓库内保留声称的 5 轮 warm-up / 30 轮采样与可复算 A/B fixture。

- `lux-cxx` RelWithDebInfo 与 Debug 均为 49/49；`lux-engine` RelWithDebInfo
  为 27/27。Debug Foundation 与全部新增 contract probes 通过；完整 Debug
  `lux-engine` 为 20/22，剩余两项是单独记账的既有 Phase 9 EnTT fixture assertion。
- Object 与 UI 的 Debug/RelWithDebInfo 安装后 consumer 分别通过独立 codegen、
  link 与 run；
  cross-DLL Base/Derived Signal prefix 布局已由真实 DLL 覆盖。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 与 Android PLAYER 全量构建通过；
  各 CMake 树第二轮均为 `ninja: no work to do`。
- RelWithDebInfo hardened contracts 的 Object/UI 定向矩阵为 13/13。
- Android NDK Clang 19 完成全量 PLAYER 编译，产物包含纯 `ui_next` 和
  `ui_next_drawdata`，不包含 GLFW 或 ImGui Vulkan backend。当前无目标设备，
  因此未宣称已执行 Android ASan/UBSan 运行时测试。
- Direct/Queued、churn、reentrancy、Pane/Command/Context 性能基线已记录，
  Direct notify 的 consumer-path 分配计数为零。
- 两个既有 Phase 9 EnTT Debug probe 仍单独记账；它们不属于
  Object/UI Foundation，也不进入本轮施工。

## 历史结论（已撤回）

- Object typed Direct 稳态路径没有 runtime owner lookup、Reflection、hash、
  Release thread-id 查询或 maintenance atomic RMW，并保持零分配。
- wrong-affinity destruction、WeakRef affinity、callback exception、inherited
  dynamic Signal 与 generated-only SignalIndex 均有永久测试和硬门禁。
- UI unchanged frame 的 Command route rebuild 为零；route change 收敛到
  `O(bindings + commands)`；输入不维护 ImGui enum 的残缺镜像。
- MSVC Debug/RelWithDebInfo、Android NDK Clang compile、cross-DLL、cross-affinity、
  hardened contracts 与 installed consumers 当时已通过，但该结果不足以冻结
  public API 与 implementation。

## 本轮完成条件

本轮完成后仅可将状态记为 `implementation stabilized pending independent audit`。
必须再经过一次独立复审，才能另行决定是否恢复 `foundation API frozen`。

## 再稳定化验收记录（2026-08-23）

- Windows RelWithDebInfo 全量 `all -j4 -k0` 与 30/30 CTest 通过，
  CMake 变更后第二轮为 `ninja: no work to do`。
- Debug Object/UI 矩阵为 17/17；完整 Debug 为 24/26，仅余两个已单独
  记账的 Phase 9 EnTT probe，不属于本轮 Object/UI 范围。
- RelWithDebInfo hardened-contract Object/UI 矩阵为 17/17。Debug 与
  RelWithDebInfo 的 Object/UI installed consumer 各自完成 codegen、link 与 run。
- 额外使用 `ENABLE_OBJECT_TEST=OFF` 与 `ENABLE_UI_NEXT_TEST=OFF` 编译
  production Object/UI targets；产物中不存在 test diagnostics 访问符号。
- DEVELOPER、PLAYER、EDITOR、TOOLCHAIN 配置通过；Android PLAYER 全量
  702/702 通过，第二轮无增量工作。
- Direct Event affinity/noexcept、Pane/Factory tombstone + WeakRef、Command dispatch
  snapshot/context ownership 均有永久测试。源码门禁确认没有 generic UI
  pending executor、旧 Object/UI API 或 Engine/Editor `ui_next` consumer 回流。
- 可复算的 5 轮 warm-up / 30 轮样本与私有 A/B fixture 已入库。
  Candidate B 未满足收益/回退阈值，因此生产布局继续使用 Candidate A。

这些结果只支持当前 `implementation stabilized pending independent audit`；
本 ADR 不宣布 public API frozen。
