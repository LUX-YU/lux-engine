# L0–L3 Architecture Gap Log

Only gaps that cannot be solved inside the active Phase public type budget belong here. An entry does not authorize an
adapter, manager, context, services bag, registry, shim or generic promotion.

| Phase | Gap | Status | Decision |
|---|---|---|---|
| 2 | Original v3.1 type budget omitted `WorldChunkReference` and `WorldPartitionTable` although its exact public shape referenced both. | Resolved | Approved repository erratum adds the two minimal public value/table types. |
| 8 | Original `WorldStorageSource` shape had no usable construction seam and an unnecessary shared `Impl`. | Resolved | Approved repository erratum uses static `create(world, read_port)` and stores the two values directly. |
| 5 | Existing `ScriptSystem` requires Asset/WorldObject/backend capabilities that the frozen `SimulationBuilder` and `Scene::create` inputs cannot supply. | Held | Keep ScriptSystem independently buildable/tested, but do not register it in P0. No Services/Context/global singleton workaround. |
| 5 | Existing Transform update requires `HierarchyMaintenance::update()` before consuming `HierarchyDeltaBatch`; registering both Transform2D and Transform3D under the one-primary-task rule would either run maintenance twice/concurrently or require an unbudgeted hierarchy System/execution point. | Held | Keep Transform Systems directly usable and tested; do not publish a misleading registration span. Resolve only through a lower-layer design review, not a wrapper. |
