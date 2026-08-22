# Unfinished Architecture Work

This ledger tracks implementation state only. Decisions live in ADRs.

## Active: semantic deduplication (2026-08-22)

- [x] Freeze the architecture boundary and legacy-vocabulary baseline.
- [x] Introduce the ECS-free typed async port and remove ECS Runtime includes.
- [x] Replace Extension ABI v4 with v5 and make Physics2D optional.
- [x] Publish reflection and component projection as one module transaction.
- [x] Remove SceneFeature, SceneContribution and Runtime Pack identities.
- [x] Reduce render extraction to one RenderSystem and a private static stage
      sequence; remove Runtime RenderEffect.
  - [x] Collapse the Renderer-internal `SceneFeature` base and container into
        the single `RenderFeature` identity. Resource-only capabilities inherit
        the default empty `addPasses`; `FeatureCatalog` remains the only
        capability graph and the per-Scene view is `RenderCapabilities`.
- [x] Move every Runtime-owned ISystem and Component to its ECS domain.
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
  - [x] Move `Infinite2DPixelSystem`, its transient domain state and typed
        preparation port into `ecs/pixel/streaming`. Runtime assets retains
        the queued preparation endpoint and procedural Section provider;
        remove the Runtime Pixel System target, spatial2d directory and
        `ISystem` allowlist entry.
  - [x] Move `Spatial3DNavigationAdapterSystem`, its typed preparation port
        and owner-thread completion inbox into `ecs/navigation/streaming`.
        Runtime assets retains the Detour endpoint and service-wide
        queued-plus-running admission budget; remove the Runtime Navigation
        System target, spatial3d directory and `ISystem` allowlist entry.
  - [x] Move `StaticCollider3DSystem`, its transient binding/status state,
        typed preparation port and Physics3D scene service into
        `ecs/physics3d/streaming`. Runtime assets retains the queued Jolt
        preparation endpoint and its process-wide budget accounting; remove
        the Runtime Physics3D System target, spatial3d directory and `ISystem`
        allowlist entry.
  - [x] Move `PrimaryViewPresentation`, its snapshot/transient binding and
        `PrimaryViewPresentationSystem` into `ecs/render/presentation`.
        Runtime render creates the ECS service and exposes its snapshot but
        owns no presentation `ISystem`; remove the final Runtime `ISystem`
        allowlist entry.
- [x] Split `engine/spatial3d/SceneCatalog` by field ownership.
  - [x] Move SourceId, cell/LOD/Section records, format limits and the frozen
        L3SC v1 codec into `ecs/scene_format/spatial3d`.
  - [x] Move residency capacity and built-in demand-channel policy into
        `ecs/spatial3d/streaming`; remove the obsolete partition Feature name.
  - [x] Move direct System assembly into `engine/runtime/scene/composition`,
        remove the `engine/runtime/spatial3d` source target and retain no old
        include, namespace or target alias.
- [x] Collapse Editor panel contribution state into UISystem registration.
  - [x] Replace descriptor catalog, activation host, command queue and tickets
        with synchronous main-thread `EditorPanels` ownership of each
        `PanelRegistration`.
  - [x] Replace the Editor contribution registrar symbol with the direct ABI
        v5 `luxInstallEditorPanelsV5(EditorPanelInstallContext&)` entrypoint;
        the Editor adapter binds every installed panel to its `ModuleLease`.
- [x] Generate build-only project usage and direct game composition.
  - [x] Aggregate canonical Component schemas, derived renderer requirements,
        Spatial3D streaming use and selected Extensions from cooked Scenes into
        `build/ProjectUsageManifest.toml`.
  - [x] Generate `build/GameComposition.cpp` with direct built-in System and
        standard Renderer assembly calls; it contains no Runtime registry and
        Physics2D remains a manifest-deployed Extension.
  - [x] Reject a Scene-required Extension not satisfied by project selection,
        compile-check generated composition, and prove neither build artifact
        is copied into the Player deployment.
- [x] Remove legacy paths/targets and set all semantic-debt limits to zero.
  - [x] Remove the empty Runtime Pack/spatial/launch/world trees and the former
        `engine/spatial3d` tree; the source gate rejects their reappearance.
  - [x] Rename product assembly targets to `runtime_scene_*_composition` and
        retire the misleading `runtime_*_systems` names without aliases.
  - [x] Run the zero-debt source gate both at configure time and through the
        fixed `lux_architecture_check` target in every `all` build.
- [x] Complete Windows profile, installed-prefix and Android validation.
  - [x] Windows `DEVELOPER`, `PLAYER`, `EDITOR` and `TOOLCHAIN` full builds
        pass; each CMake tree is stable on its required second build
        (`ninja: no work to do`). Their contract suites pass 5/5, 4/4, 4/4
        and 1/1 respectively.
  - [x] The Windows Player runtime closure contains no Physics2D Extension,
        Authoring, Toolchain, Editor, Assimp, shaderc or MLIR/LLVM link-time
        dependency.
  - [x] Android `PLAYER` configures and completes a full `all -j4 -k0` build,
        is stable on the second build and installs successfully with the
        self-contained host meta-generator runtime, current Vulkan SDK host
        tools and the native script backend. Lua remains an explicit optional
        backend on Android.
  - [x] Debug, RelWithDebInfo and Android installed public Engine headers are
        free of all retired semantic identities; the production-source scan
        has no retired identifier except the source gate's own forbidden-path
        literal.

The working tree modification under
`modules/function/input/pinclude/lux/engine/input/detail/GlfwInputTranslation.hpp`
predates this work and is not part of the refactor.
