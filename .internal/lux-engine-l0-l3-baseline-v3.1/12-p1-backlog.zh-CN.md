# P1 Backlog（v3.1，只记录；Barrier B 前不展开）

以下问题已经识别，但本 baseline 不冻结具体解法。只有 P0 probes + Barrier B 后才进入 P1：

## Streaming readiness / activation

区分何时：bytes ready、Entity materialized、physics/script/resource ready、product可结束 loading UI。

## IO saturation / concurrency

Scene不做人工 per-frame partition budget；Process/backend如何选择 native queue depth、decode concurrency、memory pressure需要 benchmark。

## AssetResidency memory policy

`lease == 0` 仅表示 reclaimable。LRU/cache/memory-pressure strategy后续。

## Multi-Scene isolation

P0已冻结 Entity/Registry Scene-local；后续讨论 shared World/Asset、cross-scene communication与 Preview/thumbnail workloads。

## Render extraction representation

全量 snapshot vs dirty ranges/SoA/chunk reuse/copy-on-write；先以 compact stable presentation representation验证。

## Determinism/replay/network

Async completion timing不能偷偷成为 deterministic Simulation input；authority activation tick、replay/prediction后续。

## Presentation interpolation/extrapolation

Slow Simulation下 authoritative-only、interpolate、extrapolate等属于 Product presentation policy。

## Plugin architecture

插件可能贡献 L1–L5 types/registrations/assets/editor functionality；module unload/code lifetime需单独设计。

## Save/Persistence

Streaming无隐式 persistence。Overlay/Snapshot/SaveGame/delta/server state等另开 subsystem design。
