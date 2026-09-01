# L1-L3 Post-Cleanup Convergence

Status: **Closed**

Baseline implementation: `48216e1b`

Date: 2026-09-01

## Result

The L1-L3 architecture remains closed after the post-cleanup hard cut. The cleanup changes physical package,
target and installation names only; it does not change System ownership, Scene metadata lifetime, Render extraction
transactions, World/Simulation/Scene wire contracts or Host-provided RenderRuntime ownership.

Scene metadata is fail-closed. Every valid component `TypeToken` referenced by Simulation access, Scene observations
or RenderFeature observations must resolve through the `ComponentSchemaSet`. An unknown schema reports
`UNKNOWN_COMPONENT_SCHEMA` with the offending token hash. RenderFeature binding identity is tracked independently
from binding contents, so a duplicate empty binding is rejected.

## Canonical topology

```text
engine/domain/
  partition/identity
  spatial/{spatial2d,spatial3d}
  system/{identity,description}
  world/{identity,partition,description,storage,asset}
  simulation/{description,asset,composition,ecs,system,builtin,scripting}

engine/process/{execution,asset_loading,world_loading}

engine/scene/
  {description,asset,system,meta,composition,presentation}
  integration/{world_materialization,render}
```

Public include and namespace names follow concepts rather than physical aggregation directories. In particular,
WorldObjectId is `lux::world::WorldObjectId`, Process workflows use `asset_loading` and `world_loading`, and builtin
Transform/Script public headers are flat under `lux/engine/simulation/`.

## Preserved contracts

- `SimulationSystem` and `SceneSystem` ownership is unchanged.
- `SceneMetaManager` is built once at startup and remains immutable.
- `RenderSystem` remains an optional SceneSystem; `RenderRuntime` remains a Host capability.
- `RenderSyncPipeline` prepare/publish/commit/discard transaction semantics are unchanged.
- World, Simulation and Scene durable wire tests remain byte-compatible.
- Headless Scene composition does not link Render, Vulkan or window products.
- Process loading packages own time-spanning workflows, not World/Asset domain ownership.

## Held work

Asset residency, Script capability injection, Product streaming, plugin hot reload and CI remain held. No Manager,
Context, Services, Registry2, Adapter, Bridge or compatibility alias was introduced by this cleanup.
