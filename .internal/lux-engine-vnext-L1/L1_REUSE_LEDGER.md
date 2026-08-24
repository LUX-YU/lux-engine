# vNext L1 reuse ledger

| 旧路径/概念 | 新 owner | 裁决 | 只复用的内容 | 新验证 |
|---|---|---|---|---|
| `ecs/core/Registry.*` | `engine/ecs/core/World.*` | REWRITE | entity generation/high-water 行为 | `vnext_ecs_core`, snapshot allocator tests |
| old unbounded/implicit change observation | World-owned Change Journal | REWRITE | cursor intent only | independent cursor, deterministic stream/block failure, pinned overflow/resync, baseline/restore and high-water intrusive block reuse tests |
| public Registry inheritance / `World::registry()` | 无 | RETIRE | 无 | negative compile probes |
| `EcsCommandBuffer` / deferred commands | `WorldCommands` + private block arenas | REWRITE | reservation/preflight、producer order | schedule command contract test |
| producer downcast commands | 无 | RETIRE | 无 | command concept 只暴露 `apply(WorldEdit&)` |
| old Schedule topology/SCC | `engine/ecs/schedule` | REUSE algorithm | stable slots、cycle/topology、reverse frontier | schedule contract, bounded wave scratch, start rollback and allocation-free close tests |
| `ScheduleBuilder`, mutation batches | `ScheduleEdit` | RETIRE/REWRITE | transaction intent | duplicate/cycle/failure rollback tests |
| `ISystem` | `System` | RETIRE | abstract update behavior | retired API compile probe |
| `SceneServices` | 未来 L3 wiring | RETIRE FROM L1 | 无 | retired API/source gate |
| component mutable catalog/global pending registrar | generated module span + `ComponentSchemaSet` | RETIRE/REWRITE | reflection descriptors | generated-schema/pin tests |
| tagged property archive | schema codec port + persistence archive | REUSE algorithm | tagged field/unknown skip | reflected codec and persistence tests |
| `RegistrySnapshot` | `WorldSnapshot` | REWRITE | backend clone idea | 10k IDs/generation/free-list plus zero-journal cold construction |
| `EntitySectionImage` | `WorldSectionImage` | REWRITE | name/schema/archetype/column/ordinal layout | LXWS deterministic/corruption and zero-journal materialize tests |
| Parent/Transform snapshot special cases | 无 | RETIRE | 无 | generic snapshot + rebuild pilot |
| old hierarchy preorder/interval | 无 | RETIRE | Parent remains canonical; linear cold validation, O(1) mutation-order adjacency and embedded orphan repair replace interval authority | 1m real graph、deep-chain/star/resync counters, injected orphan command failure and stale-generation repair tests |
| old transform dirty propagation | Transform Systems | REUSE algorithm | dirty subtree root reduction only; dense stamps/change cursors replace unordered/full scans | no-change zero-visit, leaf/root dirty and codec tests |
| asset runtime owner/cache/load ports | future L2 | RETIRE FROM L0 / DEFER | cooked byte codecs only | source/install/compile-command gate |
| old `.luxasset` v1/v2 and pak | L0 codec/VFS | REUSE WIRE | magic/version/layout/defensive limits | cooked/pak golden tests |
| shader/asset build tools in Runtime graph | future Content/Toolchain | DEFER | 无 | compile-only builtin-content seam |
| Physics/Render/Script/other ECS domains | future L1-8 | DEFER | none in this round | quarantine inventory |
