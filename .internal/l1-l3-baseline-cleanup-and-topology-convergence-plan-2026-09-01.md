# L1-L3 Baseline Cleanup and Topology Convergence

Status: implementation contract. Baseline: `a39246eeff3542af6345a96f586980e96ba1c367`.

This wave does not reopen the closed SimulationSystem/SceneSystem split, immutable SceneMetaManager, Host-owned
RenderRuntime, RenderSystem Scene capability, RenderSyncPipeline transaction, or durable World/Simulation/Scene wire.
Asset residency, product streaming, Script capability injection, plugin hot reload and CI remain out of scope.

## Required correctness changes

- SceneMetaManager must reject every valid Component TypeToken not present in ComponentSchemaSet with
  `UNKNOWN_COMPONENT_SCHEMA` and `subject_hash = TypeToken::hash()`.
- Simulation access, Scene observation and RenderFeature observation references all use the same fail-closed rule.
- RenderFeatureSceneBinding identity presence is tracked independently of payload contents; a second empty binding is
  a duplicate.

## Canonical topology

```text
engine/domain/partition/identity
engine/domain/world/{identity,partition,description,storage,asset}
engine/domain/simulation/{description,asset,composition,ecs,system,builtin,scripting}
engine/process/{execution,asset_loading,world_loading}
engine/scene/{description,asset,system,meta,composition,presentation,integration}
engine/scene/integration/{world_materialization,render}
```

Public hard cuts:

```text
lux/engine/domain/WorldObjectId.hpp -> lux/engine/world/WorldObjectId.hpp
lux::domain::WorldObjectId          -> lux::world::WorldObjectId
lux::process::asset                 -> lux::process::asset_loading
lux::process::world                 -> lux::process::world_loading
lux/engine/simulation/systems/*     -> flat lux/engine/simulation/*
```

No retired source root, target, package component, forwarding header, namespace alias or compatibility target remains.
Historical evidence retains historical paths and is excluded from current-topology gates.

## Execution order

1. SceneMeta referential integrity and negative tests.
2. Partition/World split and WorldObjectId public migration.
3. Process loading workflow rename.
4. Simulation composition/builtin rename.
5. Scene composition/presentation/integration rename.
6. SSOT, source gates, package metadata and installed consumers.
7. Default and Full Render full qualification, then restore default-OFF closure.

Every CMake/public-header wave synchronizes all three install include prefixes, builds `all -j 4 -k 0` twice, runs
the relevant tests and stages explicit files only. User `.gitignore` and WorldPartition formatting remain uncommitted.
