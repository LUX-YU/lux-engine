# ADR：Object Model 与 UI Foundation 最终加固

日期：2026-08-23
状态：Accepted（ontology frozen；public API finalized — freeze candidate；
implementation hardened；independent re-audit required；
Engine migration blocked for later redesign）

## 决策

在继续 Engine/Editor 业务重构前，先稳定两层可独立安装的基础原语：

```text
core/meta -> core/object -> function/ui
```

- `core/meta` 只描述类型、静态/实例字段、方法与构造能力；反射不拥有对象。
- `LuxObject` 只拥有对象 affinity、惰性连接状态、weak lifetime 和 targeted event。
- `Signal` 的唯一 authored identity 是 declaring C++ static member；生成的
  dense coordinate 只是 detail execution data，不是公开 Object API。
- `ObjectDispatcherRef` 是不可变的 affinity capability；message envelope 与
  post surface 全部是 Object 内部实现，不是 Executor/EventBus。
- `UISession` 是 ImGui Context、私有 dispatcher、Pane/Factory 注册、focused Context、
  `CommandRouter` 和 Layout 字节的唯一 owner；它不是 `LuxObject`。
- `Pane` 实例由具体工具以 `unique_ptr` 拥有，`UISession` 只持非 owning 注册。
- `UiContextId` 只是交互语境 identity，禁止携带
  Project/Scene/Asset/Renderer service；不再保留空 `Context` wrapper。
- `Command` 只做 focused Context 下的语义 dispatch；物理输入仍属于
  `InputContext/ActionMap/ActionMapper`。
- 同一 Pane type 的多个实例可以各自绑定相同 `Command + Context`；
  `UISession` 只让 focused Pane 的 receiver 参与 contextual dispatch，禁止未聚焦
  实例因共享 Context identity 而抢占命令。
- 动态 Object 连接只在 connect-time 查询 `RefStaticField/RefMethod` 并严格接受
  `void(const P&)`；稳态通知不查询 Reflection。Direct/零订阅 notify 不分配。
- 唯一 `function/ui` 不依赖 Input、Resource、ECS、Runtime、Renderer 或
  Editor；`DrawDataSnapshot` 是该组件内的 backend-neutral value。

## 稳定化合同

- `LuxObject` affinity 在构造时固定；connection topology 的物理变更只发生在
  sender affinity。
- receiver 析构立即阻止未来 callback；跨 affinity disconnect 先逻辑取消，再由
  sender 侧完成物理清理。
- foreign-thread disconnect 必须由 sender dispatcher 接受物理清理；dispatcher
  缺失或关闭属于 fail-fast lifetime contract violation，不能静默保留半连接。
- Direct notify 栈上同步销毁 sender 不受支持；owner 必须在 safe point 延迟销毁。
- typed Signal 使用 reference NTTP 与生成 thunk；Reflection 只参与 dynamic connect-time。
- queued Signal、posted Event 与 connection control 共享内部
  `detail::MessageEnvelope` transport；该 transport 不属于公共 Object API，
  Signal/Event 的 public semantics 保持不同。
- `UISession` 是唯一 UI session owner；Context 只表示交互语境。
- `UISession`、`CommandRouter` 与 live Registration handle 均由构造线程独占；
  跨线程 UI mutation 不通过 mutex/atomic 扩展为受支持语义。
- Command activation scope 与 handler receiver 是两个独立 weak lifetime。
- Published tuning 不锁定 `SignalIndex` 宽度、container inline capacity、SBO 大小或
  atomic 具体表示。

## 明确不引入

```text
ObjectManager / ObjectTree / EventBus
UIManager / WorkbenchManager / ContextManager / ToolHost
ContributionGraph / keyboard shortcut subsystem
```

## 当前最终收口边界

本轮删除 legacy UI，将新 Foundation 直接收口为唯一
`modules/function/ui`。Object/UI ontology 保持冻结，public API 进入最终
finalization，implementation 执行最后 correctness/performance hardening。
Engine/Editor 业务 consumer 在旧 UI 删除后明确处于不可构建状态，必须
留待后续专项重构；本 ADR 不提供兼容层或临时 target。

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
- UI unchanged frame 的 Command route rebuild 为零；route change 的真实复杂度为
  `O(bindings × active-context-count + commands)`，active contexts 通常为 1～8，
  因此不为形式上的 `O(B+C)` 引入 hash 索引；输入不维护 ImGui enum 的残缺镜像。
- MSVC Debug/RelWithDebInfo、Android NDK Clang compile、cross-DLL、cross-affinity、
  hardened contracts 与 installed consumers 当时已通过，但该结果不足以冻结
  public API 与 implementation。

## 最终加固完成条件

本轮只验收 `core/object` 与唯一 `function/ui` 的 modules-only 矩阵。
不构建、不宣称 Engine/Editor 通过。完成后状态固定为：

```text
Ontology Frozen
Public API Finalized
Implementation Hardened
Independent Audit Pending
Engine Migration Blocked
```

即使 modules-only 矩阵全绿，也不自动宣布 `Foundation Public API Frozen`。

## 最终加固验收记录（2026-08-23）

- H0–H6 已完成。Windows MSVC Debug、RelWithDebInfo 与 hardened-contract
  配置均只构建 `object`、`ui`、meta sidecar 和对应测试；每个构建树的第二轮
  都是 `ninja: no work to do`。
- RelWithDebInfo 为 Object 11/11、UI 6/6；Debug 与 hardened-contract 均为
  Object 14/14、UI 6/6。负向编译、wrong-affinity、cross-DLL、release benchmark
  和 draw-data move fixup 均包含在这些计数中。
- Android PLAYER 使用 NDK Clang 完成 `object` 与唯一 `ui` target 编译，第二轮
  无增量工作。Debug/RelWithDebInfo 的 Object/UI installed consumer 分别完成
  codegen、compile、link 和 run。
- tests-off production DLL 不导出 `ForTest`、diagnostics access 或 message-storage
  计数符号。三个安装前缀只保留最终 `object` 与 `ui` 公开头、库和组件 metadata；
  被退役的安装产物已精确移入可恢复的 build quarantine。
- Release 证据保留 5 次 warm-up、30 次正式样本、raw CSV、median、p95、真实 DLL
  storage diagnostics 和 A/B fixture。Candidate B 的 4/16/64 几何平均提升约
  7.4%，未达到 10%，且 churn 明显回退，因此 Candidate A 仍是唯一生产布局。
- Source gate 固化唯一 `function/ui`、禁止 legacy/next target 与 public execution
  detail 回流，并把 Engine/Editor consumer migration 明确留给后续重新设计。

本轮最终状态为：

```text
Ontology Frozen
Public API Finalized
Implementation Hardened
Independent Audit Pending
Engine Migration Blocked
```

该状态仍不等于 `Foundation Public API Frozen`。

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

## 独立审计整改后的冻结候选状态（2026-08-24）

独立审计认可 ontology，不要求新的架构切割。本轮只处理冻结后难以回收的边界：

- foreign-thread Connection cleanup 和立即清空 handle 的生命周期合同；
- UI owner-thread contract 与 frame 状态机的全配置 fail-fast；
- contextual route 只由 UISession 修改，Command 查询收窄为即时 label view；
- 删除 Menu/Toolbar 单字段 wrapper，使用 invariant-safe item factory；
- LayoutSnapshot 值语义、Object detail header、SignalView 和 dynamic error 收口；
- Pane Signal callback 只能请求延迟销毁，不能同步销毁 sender。

本轮不实施 Candidate C，不迁移 Engine/Editor consumer。验证完成后的状态固定为：

```text
Ontology Frozen
Public API Finalized — Freeze Candidate
Implementation Hardened
Independent Re-audit Required
Engine Migration Blocked
```

只有后续独立复审签字，才能改为 `Foundation Public API Frozen` 并解除迁移阻塞。

## 冻结候选验收记录（2026-08-24）

- RelWithDebInfo Object/UI 为 16/16 + 16/16；Debug 与 hardened-contract 均为
  19/19 + 18/18。三套 Windows modules-only 构建的第二轮均为
  `ninja: no work to do`。
- Android NDK Clang 完成 `object`、`ui` 与 architecture gate，第二轮无增量工作。
  Debug、RelWithDebInfo、Android 三个安装前缀的 Object/UI 公开头与源码哈希一致，
  且均不存在退役的 `ObjectFwd.hpp`。
- Debug 与 RelWithDebInfo installed consumer 均重新完成 Object meta codegen、编译、
  链接和运行。tests-off production DLL 不导出 test diagnostics、`ForTest` 或
  owner-contract helper 符号。
- 性能使用相同 MSVC 配置对 `8d60dba9` 与候选实现进行交错采样，每个 case 为
  5 次 warm-up + 30 次正式样本。两组对照中，Object Direct 聚合 median/P95 为
  `+2.62/+2.06%`、`-1.74/+2.17%`；UI steady frame 为 `-0.01/-3.21%`、
  `+2.73/-8.36%`；UI state 为 `+2.14/+0.48%`、`+3.47/+3.65%`；UI invoke 为
  `+0.16/+0.24%`、`+2.85/+4.01%`。受约束热路径均在 5% 门限内，稳态分配为零；
  原始 CSV 保留在 RelWithDebInfo Object/UI 构建树。
- Engine/Editor consumer、产品 profile 和 `ui_vulkan` 不属于本次验收，也未被迁移。
  它们继续服从 `Engine Migration Blocked`，等待后续独立重构方案。

因此本轮只进入 `Freeze Candidate / Independent Re-audit Required`，不提前签署
`Foundation Public API Frozen`。
