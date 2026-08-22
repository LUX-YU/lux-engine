# Directory / Target / Product Architecture

Status: authoritative  
Decision date: 2026-08-22

## Dependency spine

```text
PLATFORM -> CORE -> RESOURCE -> FUNCTION -> ECS -> RUNTIME -> HOST
                                      AUTHORING -> TOOLCHAIN -> EDITOR
```

The source tree expresses the same ownership:

```text
modules/ -> ecs/ -> engine/ -> hosts/products
```

- `modules/` owns reusable mechanisms that do not know Entity or Component.
- `ecs/` owns World facts, all Entity/Component-aware domain behavior and the
  only System topology.
- `engine/` owns product lifecycle, loading, authoring, cooking, extension
  loading, editor composition and hosts. Runtime orchestration may operate on
  a World but must not define domain Components or Systems.
- `extensions/` contains deployable leaves. Engine production targets never
  link a concrete extension.

## Unique identities

- Component: World fact.
- `ISystem + Schedule`: only behavior graph.
- `RenderFeature + FeatureCatalog`: only renderer capability graph.
- Module + `ModuleLease`: code availability and lifetime.
- `SceneDescription`: cooked entity/component data and derived requirements.

`SceneFeature`, `SceneContribution`, Runtime `RenderEffect`, Runtime Pack,
System Registry and installer/catalog/host variants are forbidden.

## Directory rules

- ECS domain integration stays with its domain; `ecs/integration` is reserved
  for code with two equal domain owners.
- Runtime execution owns queue/thread/scheduler implementations. ECS consumes
  only narrow modules-level ports.
- `ecs/entity_scene` owns Section decode/stage/materialization and the
  publication Systems. Its `residency/` namespace owns the dimension-neutral
  demand union, budget planner and `EntitySectionResidencySystem`; these
  borrow canonical `SectionRecord` spans and do not depend on Engine catalogs.
  `engine/runtime/entity_scene` implements the typed load endpoint, generator
  execution and concrete content-blob storage; it defines no `ISystem`.
- `engine/runtime/spatial_partition` and the `runtime_spatial_partition`
  target do not exist. Dimension-specific interest producers belong under
  their ECS `streaming/` domains, not in a new Runtime partition layer.
- `ecs/spatial2d/streaming` owns 2D Section addressing and interest-to-demand
  behavior. Runtime Pixel consumers may query its activity but do not own or
  redefine that System.
- `ecs/spatial3d/streaming` owns 3D Section catalog/rule sources and
  interest-to-demand behavior. The remaining Runtime spatial3d installer is
  only a product adapter from the current Engine cooked catalog; it must move
  or disappear when `engine/spatial3d/SceneCatalog` is split by field owner.
- `ecs/tilemap/streaming` owns Tilemap chunk observation, preparation intent,
  publication, activity and retirement. `engine/runtime/assets/tilemap`
  implements only the typed background decode endpoint and queue policy; the
  ECS System consumes its `OperationPort` through an owner-thread inbox.
- Runtime render owns backend/session/frame lifetime. Renderer mechanisms stay
  in `modules/function/render`; extraction stays in `ecs/render`.
- Shared cooked contracts may remain in `engine/scene` or `ecs/scene_format`;
  being consumed by Runtime does not make a format Runtime implementation.
- Extension ABI remains in `engine/extensions/api`; dynamic loading and leases
  belong in `engine/runtime/extensions/loader`.
- Directory moves happen after semantic ownership is established. Moves do not
  leave forwarding headers, namespace aliases or target aliases.

## Build products

- `DEVELOPER`: all development products.
- `PLAYER`: runtime-clean player and reference host.
- `EDITOR`: runtime, toolchain and editor; player closure remains clean.
- `TOOLCHAIN`: offline transforms and their explicit schema dependencies.
- Android uses `PLAYER` with its toolchain/triplet.

All production targets call `lux_classify_target`. Build-tool dependencies use
generated files/custom commands and never become Runtime link dependencies.
