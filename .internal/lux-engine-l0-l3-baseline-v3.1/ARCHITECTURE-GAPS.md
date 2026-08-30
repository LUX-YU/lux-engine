# L0–L3 Architecture Gap Log

Only gaps that cannot be solved inside the active Phase public type budget belong here. An entry does not authorize an
adapter, manager, context, services bag, registry, shim or generic promotion.

| Phase | Gap | Status | Decision |
|---|---|---|---|
| 2 | Original v3.1 type budget omitted `WorldChunkReference` and `WorldPartitionTable` although its exact public shape referenced both. | Resolved | Approved repository erratum adds the two minimal public value/table types. |
| 8 | Original `WorldStorageSource` shape had no usable construction seam and an unnecessary shared `Impl`. | Resolved | Approved repository erratum uses static `create(world, read_port)` and stores the two values directly. |
| 5 | Existing `ScriptSystem` requires Asset/WorldObject/backend capabilities that the frozen `SimulationBuilder` and `Scene::create` inputs cannot supply. | Held | Keep ScriptSystem independently buildable/tested, but do not register it in P0. No Services/Context/global singleton workaround. |
| 5 | Existing Transform update requires `HierarchyMaintenance::update()` before consuming `HierarchyDeltaBatch`; registering both Transform2D and Transform3D under the one-primary-task rule would either run maintenance twice/concurrently or require an unbudgeted hierarchy System/execution point. | Held (domain-local) | The 2026-08-30 Transform+Jolt review preserves one-primary-task: real Jolt uses one node, so there is no repeated multi-node evidence. Keep Transform registration Held until hierarchy maintenance has one concrete lower-domain owner; do not widen the Builder. |
| 10 | Jolt double-position preserves body/narrow-phase precision at `1e12`, but its float broadphase loses meter-scale separation and can create excessive candidate pairs for dense far-origin populations. | Held | The preloaded 3D slice must measure concrete spatial partitioning. Do not introduce a persistent physics origin, body rebase, runtime precision variant or generic framework. |
