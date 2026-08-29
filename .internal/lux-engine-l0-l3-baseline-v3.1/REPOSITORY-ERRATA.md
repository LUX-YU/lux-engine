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
