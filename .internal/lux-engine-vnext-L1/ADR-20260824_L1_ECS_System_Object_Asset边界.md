# ADR：vNext L1 ECS、Object 与 Asset 边界

- 日期：2026-08-24
- 状态：Accepted boundary；mutation/schedule/hierarchy 部分由 L1 v2 ADR 修订
- 适用范围：当前 `modules/` 与 `engine/ecs/`
- 替代范围：旧文档中的顶层 `ecs/`、`ISystem`、`SceneServices`、
  `ScheduleBuilder`、runtime `AssetManager` 方案

> 本文中的 persistence wire/codec 陈述已由
> `ADR-20260824_Foundation_TypeInfo_Serialization_LXWC.md` 覆盖。

## 裁决

1. `World` 是 canonical ECS data owner。它私有持有 EnTT backend；runtime
   `Entity` 只在一个 World 内有效，不能持久化。L1 v2 进一步要求 component
   非结构修改也必须经过显式 Read/Write capability，并由 Lux Change Journal
   记录；EnTT signal 不再是 canonical change truth。
2. 一个 World 同时最多绑定一个 live `Schedule`。Schedule 是 System 的唯一
   owner 和唯一行为图；Signal/Event 不构造第二套执行图。
3. `System` 与 `LuxObject` 正交。Schedule 只识别通用 execution-affinity
   protocol；具体 `add<T>` 实例化点生成擦除 thunk。Schedule target 不认识也不
   链接 Object runtime。
4. Object Event handler 只能写 System-local inbox。canonical World 改变只能发生在
   后续 `System::update` 产生的 `WorldCommands`，并在 tick barrier 后可见。
5. L1 不提供 `SceneServices`，不依赖 L2。同步依赖由具体 System 构造函数显式借用；
   异步语义由未来 L3 通过 Object Signal/Event wiring 组合。
6. Snapshot 与 persistence 不保存 System、Object identity、relation、inbox、handle、
   slot、topology 或外部引用。`WorldSnapshot` 是同进程/同构建 clone；`LXWS v1`
   是 portable、little-endian、versioned world-section image。
7. 活跃 L0 asset 只有 soft identity、immutable codec descriptors、cooked wire
   inspection、pak 与同步 VFS mechanism。runtime owner/cache/refcount/load/event
   orchestration 属于未来 L2，活跃 L0 不留 stub、alias 或 wrapper。
8. `.luxasset` 与 pak wire 保持兼容；旧 C++ API 与旧 ECS wire (`LXES`) 不兼容。

## 不变量

- `legacy/` 不 configure、compile、install、link、codegen 或 package。
- 所有 L1 target（包括 schedule）不依赖 Object 或 `engine/process`。
- EnTT observer/attach/detach 模型在 v2 退休；System 通过 ChangeCursor 消费变更。
- barrier 合并顺序是 compiled System order + producer-local sequence；writer 在
  update scope 结束时失效，apply phase 是封闭 mutation phase。
- restore 只接受 Idle、无 Schedule、无 observer relation 的 World。
- hierarchy/transform 是本轮唯一 domain vertical slice。

## 后果

未来 L2 可以重写 `AssetStore/AssetClient/AssetLease/TaskSystem`，未来 L3 负责把它们
与 Object System 的 semantic messages 连接，而无需改变 L1 数据、调度或 wire。
Physics、Render、Script 等 domain 必须在 L1-8 分别审计，不能恢复旧 integration pack。
