# ADR：L1 v2 Mutation、Schedule 与 Hierarchy 重稳定

- 日期：2026-08-24
- 状态：Accepted for implementation；Freeze pending independent audit
- 基线：`0a83e8be`

## 裁决

1. World 是唯一 component data truth。公开 World 只能读取；WorldEdit 和
   SystemFrame 的显式 Write capability 是唯一非结构修改入口，WorldEdit 与
   WorldCommands 是唯一结构修改入口。
2. Lux Change Journal 记录 component/entity storage change。它由 World 拥有，
   使用 bounded memory、epoch/sequence cursor 和 overflow resync；它不是 undo
   log、EventBus、Snapshot 或 persistent data。
3. `WorldConfig` 只配置 World runtime mechanism。Snapshot instantiate 与 LXWS
   materialize 由 caller 提供配置；restore 保留 destination policy。clone/load
   完成后建立 fresh journal baseline，不传播历史。
4. Schedule 分别维护 execution DAG、access conflict 与 lifetime DAG。Phase 是
   execution band，before/after 只排序，requires 才建立 provider lifetime。
5. Schedule 只消费通用 thread-affinity protocol。Object System 的 validator
   thunk 在 concrete `add<T>` TU 生成；pure schedule 没有 Object binary closure。
6. Parent 是 hierarchy 唯一 canonical truth。HierarchyIndex 是增量 intrusive
   adjacency cache；Transform 只消费 local/hierarchy change streams并处理 dirty
   subtree。
7. Snapshot policy、persistent codec、reflection projection 是独立事实。

Schema 的安装边界据此拆成 `lux::engine::ecs::schema` 与可选的
`lux::engine::ecs::schema_reflection`。前者只定义 schema、operations、codec port
与 snapshot policy，不 include/link meta 或 reflection runtime；后者拥有
`RefClass` adapter、默认 reflected tagged codec 与 generator projection。
`Copy/Rebuild × codec present/absent` 四种组合都合法，codegen 分别读取
`snapshot` 与 `codec` annotation，不再通过一个宏隐含绑定两项 policy。

Snapshot instantiate 与 LXWS materialize 显式接收 `WorldConfig`；cold restore
保留 destination config 和已分配 journal blocks，只替换 canonical registry，随后
递增 epoch 并建立 fresh baseline。Snapshot capture、WorldSection build 与
PersistentEntityIndex build 都要求 construction-owner thread 上的 Idle World。
LXWS v1 和 TaggedProperty 的结构字段使用逐字节 LE primitives；Tagged writer
直接写 destination 并回填 property count，不再保存第二份 Property payload。

Hierarchy v2 不再公开 `rebuild/preorder/subtree/setEdge`，也不在 mutation helper
中先改 derived cache。`reparent/detach/destroySubtree` 只验证并修改 Parent；
`HierarchySystem` 从 Parent/Entity journal 增量维护 generation-aware intrusive
adjacency。children range 无分配，hierarchy change stream 固定 65,536 records；
首次同步或上游 cursor resync 时先在 temporary state 完整校验，再原子 swap。

Transform v2 只消费各自 Local component journal 与 HierarchyIndex change stream。
无变化帧在 query 前退出；变化帧以 dense generation stamps 合并 dirty roots，并只用
可复用 vector traversal stack 访问受影响 subtree。已有 WorldTransform 通过
SystemFrame 的 declared write capability 原地更新，缺失或应删除的 derived
component 通过 phase-end commands 完成。3D persistent decode 拒绝零四元数，并在
写入 canonical Transform3D 前规范化所有有效四元数。

Schedule v2 的 lifetime edge API 命名为 `ScheduleEdit::require(consumer,
provider)`；设计文档中的伪代码 `requires(...)` 在 C++ 中是保留关键字，不能作为
成员函数标识符。该命名差异不改变“只有 hard requirement 才进入 lifetime DAG”的
语义。

## 重稳定实施结果

Change Journal 最终实现为一个 World-wide 4 KiB reusable block arena。每个
component/entity stream 只保存 block 指针环；全局按 oldest unpinned block 回收，
cursor 只保存 epoch/sequence。固定 cursor 导致暂时无法回收时，stream 在 unpin 后
建立新 baseline，确保 bounded policy 不退化为隐藏的无界分配。

ReadQuery 直接包装 EnTT const `view.each()` iterator，不在每个 entity 上重新执行
registry lookup。100k/1m 的 30-sample median 都优于对应 raw EnTT fixture，满足约
5% overhead gate。Snapshot 1m 从早期诊断版本的非线性 journal copying 修正为
88.789/97.090/94.916 ms capture/instantiate/restore median。

ECS component annotation vocabulary 最终由 `ecs::schema` 的独立安装头
`ComponentAnnotations.hpp` 提供。它只在 meta parse-time 展开 annotation，不带
meta/reflection runtime 类型或 binary closure。Android 的分离安装前缀验证了
Parent/Transform 不再依靠 Windows 聚合 include prefix 偶然找到
`MetaAnnotations.hpp`。

本 ADR 的实现矩阵已完成，但公开 API 状态只提升为 Freeze Candidate；独立审阅
仍是冻结的必要条件。

## Stop line

L1 v2 完成完整 correctness/performance/install/cross-platform 矩阵后只能成为
Freeze Candidate。独立审阅前不得迁移 Render、Physics、Animation、Script、
Streaming，也不得在不稳定 World/Schedule API 上建立 L2 AssetStore 或 L3 Scene。
