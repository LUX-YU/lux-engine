# Repository Errata for Baseline v3.1

本文件记录针对目标仓库 `230374a5f0d53e52bbb5d3bdce33cac62da06660` 已批准、并已直接同步到
canonical 文档正文的勘误。

## Erratum 1 — Phase 2 public type budget

原文的 public type list 漏掉了正文精确 public shape 已引用的：

```text
WorldChunkReference
WorldPartitionTable
```

两者已加入 Phase 2 budget。前者仅保存 volume/chunk ordinal；后者仅保存并查询 paged partition
table directory。

## Erratum 2 — WorldStorageSource representation

删除原文的 private shared `Impl`。有效 Source 由静态 `create(world, read_port)` 构造，实例直接保存：

```text
shared_ptr<const WorldDescription>
OperationPort<ReadWorldStorageRange>
```

Provider-specific state 由 `OperationPort` Endpoint 私有持有。`create()` 只验证非空 world 和有效 port。

## Erratum 3 — L1 shared partition identity and Process workflow placement

后续runtime-foundation review批准了窄义 `DOMAIN` classifier。`PartitionOrdinal` 与
`PartitionIndexTypeId` 下沉到中立 `engine/domain/partition`，避免 Simulation 依赖 World，也避免把
room/portal 等partition index误分类为Spatial。World继续拥有durable partition identity、table、descriptor与
opaque artifact。

Phase 8 的storage workflow从Scene下沉到 `engine/process/world`；`engine/process/execution` 仍严格领域盲。
Scene只保留World data到Registry的materialization/adoption。Asset typed load Sender位于独立
`engine/process/asset`，不授权Asset lifecycle/residency manager。
