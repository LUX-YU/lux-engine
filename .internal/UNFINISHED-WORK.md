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
  - [x] Aggregate canonical Component schemas and Spatial3D streaming use from
        cooked Scenes, renderer roots from product composition, and selected
        Extensions into `build/ProjectUsageManifest.toml`.
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

# Active: Object / UI foundation correctness and evidence restabilization (2026-08-23)

- [x] Enforce direct Event affinity and a uniform noexcept notification boundary.
- [x] Make UISession Pane/Factory mutation reentrant-safe and make non-owning Pane
      lifetime observable through ObjectWeakRef.
- [x] Snapshot Command dispatch before user callbacks and own route Context storage.
- [x] Replace the invalidated single-sample performance report with reproducible
      5-warm-up/30-sample fixtures and retained private A/B evidence.
- [x] Re-run the complete build/test/install matrix; leave API freeze pending an
      independent audit.

Validation: RelWithDebInfo is 30/30 with a second no-work build; Debug Foundation
is 17/17 and hardened Foundation is 17/17. Full Debug remains 24/26 only because
the two separately tracked Phase 9 EnTT probes still fail. DEVELOPER, PLAYER,
EDITOR, TOOLCHAIN and Android PLAYER build matrices pass; the standard Android
tree completes 702/702 and is no-work on its second build. The resulting status
is `implementation stabilized pending independent audit`, not API frozen.

## Previous performance stabilization record (freeze conclusion withdrawn)

- [x] Enforce callback, affinity, WeakRef, dispatcher and inherited-Signal contracts.
- [x] Remove typed Signal runtime owner lookup and steady maintenance RMW.
- [x] Make SignalIndex generated-only and deduplicate connection identity.
- [x] Benchmark listener layout against the locked relative-performance threshold.
- [x] Make Command routing change-driven and linear in bindings plus commands.
- [x] Remove redundant UI wrappers/key mirrors and eliminate common drag/drop copies.
- [x] Re-run the complete build/test/install matrix before restoring
      `foundation API frozen`.

## Previous stabilization baseline

- [x] Replace authored Signal names/address routing with generated SignalIndex and
      reference-NTTP typed APIs.
- [x] Replace shared-slot Object lifetime with one intrusive ObjectState, sender-affine
      connection mutation and receiver incoming invalidation.
- [x] Replace the generic dispatcher task queue with ObjectMessage-only capability
      transport and immutable Object affinity.
- [x] Stabilize dynamic connect-time Reflection, cross-affinity and cross-DLL behavior.
- [x] Remove the UISystem/generic-post wrappers; stabilize Pane/Context/Command hot paths.
- [x] Replace the PoC draw-data target with backend-neutral `ui_next_drawdata` without aliases.
- [x] Complete installed-consumer, benchmark and build/test matrices before restoring
      the `foundation API frozen` status.

## Superseded PoC completion record

- [x] Extend Reflection projection with static data members, owner/type data,
      method annotations and constructibility-aware object operations.
- [x] Add `modules/core/object` with lazy object state, stable typed Signals,
      Connection/WeakRef, owner-thread Dispatcher and targeted Events.
- [x] The superseded PoC added independent UI core and draw-data targets without
      changing legacy UI or Editor business wiring; the draw-data target has since
      been replaced by `ui_next_drawdata` without an alias.
- [x] Add dependency-direction gates and focused Object/UI foundation tests.
- [x] Add dynamic reflected Signal-to-method connection, address identity and
      allocation contracts for zero-subscriber/direct notification.
- [x] Split contextual semantic dispatch into `CommandRouter`; complete Pane
      focus/visibility/local Context, Menu/Toolbar, Factory and Layout models.
- [x] Make compact Reflection IR v2 the public parser result and generator
      transport; reject the retired binary v1 representation.
- [ ] Remove the parser's remaining private legacy AST construction and the
      generator's private legacy template view after all templates and external
      consumers have moved to direct compact-IR queries. Neither is a public
      canonical model anymore.
- [x] Validate isolated Object/UI installed consumers plus DEVELOPER, PLAYER,
      EDITOR, TOOLCHAIN and Android PLAYER builds. RelWithDebInfo suites pass
      27/27 for lux-engine and 49/49 for lux-cxx; lux-cxx Debug passes 49/49;
      all Windows profiles and Android PLAYER report `ninja: no work to do`
      on their second build.
- [ ] Repair the two pre-existing Phase 9 probe fixtures that assert in Debug
      on duplicate EnTT component insertion (`render_subsystem_probes` and
      `render_subsystem_lifecycle_probe`). The new Object/UI focused Debug tests
      pass; this does not block the frozen foundation API, but remains a full
      Debug-suite baseline defect.

# Phase 9 semantic de-duplication finalization (validation active)

- [x] Replace `RenderSystemStages` with an immutable private Stage vector and
      classify independently owned render behavior as ordinary `ISystem`.
- [x] Keep renderer requirements on concrete render-facing implementations and
      collect them in cold product recipes without changing `ISystem`.
- [x] Make `SceneServices` own a sealed `SceneRenderBinding`; make
      `RenderSystem` own and close the `RenderSceneLease`.
- [x] Make Schedule close a reverse-topology completion frontier and remove
      SceneRuntime's per-System/integration close state.
- [x] Remove `ISceneRuntimeIntegration`, `EntitySceneCatalog` and
      `EntitySectionRecordStore`; SceneRuntime directly owns immutable
      `SceneDescription` storage before Systems borrow it.
- [x] Remove Scene renderer-requirement fields in LXSC v3 and reject v2.
- [x] Decouple HeightFog from Water without adding an Atmosphere capability.
- [x] Collapse generated Reflection/Component registration into one module
      pending draft and validate-before-publish batch.
- [x] Preserve semantic ID ownership, retain distinct WorldActor/Entity IDs,
      and replace long-lived authoring adapters with explicit conversions.
- [x] Add zero-debt gates and permanent characterization/transaction tests.
- [x] Complete the final full Windows/profile/Android build and CTest matrix
      for the Phase 9 working tree.
  - [x] Windows `DEVELOPER`, `PLAYER`, `EDITOR` and `TOOLCHAIN` full builds
        pass, their required second builds report `ninja: no work to do`, and
        CTest passes 16/16, 8/8, 8/8 and 3/3 respectively.
  - [x] Android `PLAYER` completes the full 1069-step cross build, its second
        build reports `ninja: no work to do`, and installation succeeds. The
        host-generated metadata prefix contains only the single-draft capture
        path (`queueGeneratedComponent` count 0).
  - [x] The final architecture report records zero occurrences for every
        retired semantic identity and the Developer Phase 9 suite passes
        11/11.
  - [x] Debug, RelWithDebInfo and Android installed public headers are
        byte-synchronized for the changed contracts; all 39 retired installed
        headers were removed, the retired-header scan is zero, and the Scene
        Asset installed consumer configures, links and runs successfully.
