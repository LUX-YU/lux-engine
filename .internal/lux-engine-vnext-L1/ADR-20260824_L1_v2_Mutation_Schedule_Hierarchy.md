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

Schedule v2 的 lifetime edge API 命名为 `ScheduleEdit::require(consumer,
provider)`；设计文档中的伪代码 `requires(...)` 在 C++ 中是保留关键字，不能作为
成员函数标识符。该命名差异不改变“只有 hard requirement 才进入 lifetime DAG”的
语义。

## Stop line

L1 v2 完成完整 correctness/performance/install/cross-platform 矩阵后只能成为
Freeze Candidate。独立审阅前不得迁移 Render、Physics、Animation、Script、
Streaming，也不得在不稳定 World/Schedule API 上建立 L2 AssetStore 或 L3 Scene。
