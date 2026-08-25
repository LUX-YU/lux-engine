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
| component mutable catalog/global pending registrar | generated module span + `ComponentSchemaSet` | RETIRE/REWRITE | stable identity and runtime clone operations only | generated-schema/pin tests |
| `ComponentCodec` / reflected tagged property archive | no schema owner; retired | RETIRE | none; generic static Serializer replaces field codec tables | architecture gates and column persistence tests |
| object/field-level erased serialization | `world_section` load thunk | RETIRE/REWRITE | one type-erasure boundary per loaded Component column | dispatch/storage-lookup counters and static/plugin binding tests |
| `RegistrySnapshot` | `WorldSnapshot` | REWRITE | entity allocator clone semantics | storage-call counters, zero `has()` hot loop, 10k/100k/1m allocator tests |
| `EntitySectionImage` / LXWS / LXWC v1 | `world_section` / LXWC v2 | RETIRE/REWRITE | checked LE parsing and column/ordinal semantics only; archetype/entity DTOs retired | structural fuzz/corruption, transactional load/unload and 1m scaling tests |
| `WorldSectionWriter::build(World, ...)` | future L4/L5 Cook | RETIRE FROM L1 | no production code; deterministic storage traversal intent only | L1 gate rejects World-to-bytes and Cook dependencies |
| `ComponentPersistenceBinding` / `persistence_contract` | `ComponentLoadBinding` in `world_section` | RETIRE/REWRITE | automatic typed column thunk and code pin | immutable load-set, one-call-per-column and installed extension consumer tests |
| byte-shaped fixed Schedule scratch | adaptive record-count lane arena | RETIRE/REWRITE | history-loss fallback only | 1M complete/incomplete-access write, zero-overflow and bounded-cap probes |
| load/unload global history loss | lexical exact change publication | RETIRE/REWRITE | pin-safe loss epoch only for allocation failure | live 1M resident + 10K section reconciliation, zero normal history loss |
| Entity × all World storages unload scan | section membership ledger | RETIRE/REWRITE | none | gameplay-added Component unload and linear complexity counters |
| implicit Component snapshot/load policy | explicit generated COPY/REBUILD × LOAD/OMIT projections | RETIRE/REWRITE | stable schema identity only | codegen-stage missing-policy rejection and four-combination tests |
| Parent/Transform snapshot special cases | 无 | RETIRE | 无 | generic snapshot + rebuild pilot |
| old hierarchy preorder/interval | 无 | RETIRE | Parent remains canonical; linear cold validation, O(1) mutation-order adjacency and embedded orphan repair replace interval authority | 1m real graph、deep-chain/star/resync counters, injected orphan command failure and stale-generation repair tests |
| old transform dirty propagation | Transform Systems | REUSE algorithm | dirty subtree root reduction only; dense stamps/change cursors replace unordered/full scans | no-change zero-visit, leaf/root dirty and codec tests |
| asset runtime owner/cache/load ports | future L2 | RETIRE FROM L0 / DEFER | cooked byte codecs only | source/install/compile-command gate |
| old `.luxasset` v1/v2 and pak | L0 codec/VFS | REUSE WIRE | magic/version/layout/defensive limits | cooked/pak golden tests |
| shader/asset build tools in Runtime graph | future Content/Toolchain | DEFER | 无 | compile-only builtin-content seam |
| Physics/Render/Script/other ECS domains | future L1-8 | DEFER | none in this round | quarantine inventory |
