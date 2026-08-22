# Unfinished Architecture Work

This ledger tracks implementation state only. Decisions live in ADRs.

## Active: semantic deduplication (2026-08-22)

- [x] Freeze the architecture boundary and legacy-vocabulary baseline.
- [x] Introduce the ECS-free typed async port and remove ECS Runtime includes.
- [x] Replace Extension ABI v4 with v5 and make Physics2D optional.
- [x] Publish reflection and component projection as one module transaction.
- [ ] Remove SceneFeature, SceneContribution and Runtime Pack identities.
- [ ] Reduce render extraction to one RenderSystem and a private static stage
      sequence; remove Runtime RenderEffect.
- [ ] Move every Runtime-owned ISystem and Component to its ECS domain.
  - [x] Move EntitySection decode/stage/materialization, loader and startup
        publication into `ecs/entity_scene`; Runtime retains only the typed
        endpoint, generator execution and concrete blob storage.
  - [x] Move the dimension-neutral EntitySection residency union and its
        Schedule owner into `ecs/entity_scene/residency`; the planner now
        borrows a canonical `SectionRecord` span instead of depending on a
        Runtime catalog. Remove `runtime_spatial_partition` and its Runtime
        `ISystem` allowlist entry.
  - [x] Move the 2D Section index/source and `SpatialInterest2DSystem` into
        `ecs/spatial2d/streaming`; remove `runtime_spatial2d_infinite` and its
        Runtime `ISystem` allowlist entry. Runtime Pixel code is now only a
        consumer of the ECS streaming target.
  - [x] Move the 3D Section catalog/rule source and
        `SpatialInterest3DSystem` into `ecs/spatial3d/streaming`; remove
        `runtime_spatial3d_partitioned` and its Runtime `ISystem` allowlist
        entry. The Engine cooked-catalog adapter remains pending the field
        ownership split below.
  - [x] Move `TilemapChunkSystem`, its transient domain state and typed
        preparation port into `ecs/tilemap/streaming`. Runtime assets retains
        only the queued decode endpoint; remove the Runtime Tilemap System
        target, directory and `ISystem` allowlist entry.
  - [ ] Move pixel, physics, navigation and presentation Systems from Runtime
        into their ECS domains.
- [ ] Split `engine/spatial3d/SceneCatalog` by field ownership.
- [ ] Collapse Editor panel contribution state into UISystem registration.
- [ ] Generate build-only project usage and direct game composition.
- [ ] Remove legacy paths/targets and set all semantic-debt limits to zero.
- [ ] Complete Windows profile, installed-prefix and Android validation.

The working tree modification under
`modules/function/input/pinclude/lux/engine/input/detail/GlfwInputTranslation.hpp`
predates this work and is not part of the refactor.
