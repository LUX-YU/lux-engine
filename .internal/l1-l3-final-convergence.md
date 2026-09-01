# L1-L3 Final Convergence

Status: implemented and qualified on 2026-09-01.

## Frozen ownership

- `SimulationSystem` owns authoritative synchronous Simulation behavior and contributes to the Simulation TaskGraph.
- `SceneSystem` owns L3 cross-domain composition and contributes at most one stable-point hook and one Presentation hook.
- `SceneMetaManager` is the immutable startup query graph for component schemas, SimulationSystem registrations,
  SceneSystem registrations and backend-neutral RenderFeature metadata. It has no hot mutation/snapshot API.
- `RenderSystem` is the concrete builtin SceneSystem `lux.builtin.system.render` and is single-per-Scene.
- `RenderRuntime` is the sole L3-facing Host capability for render control/program/upload endpoints and the live
  `FeatureCatalog`. Its lease does not prescribe when a Host stops its backend.
- `RenderSyncPipeline` is an internal retained-state handoff mechanism, not a System or thread owner.

## Render composition

The durable `RenderSystemConfiguration` stores versioned `FeatureTypeId` values and portable reflected feature
configuration bytes. Installation resolves the live catalog dependency closure, materializes exact attach wire only
on the cold path, creates feature-owned extraction stages through `RenderFeatureSceneBinding`, then discards the cold
`FeatureBindings` draft. Runtime state retains only the Runtime lease, RenderScene lease and RenderSyncPipeline.

No selected RenderSystem means no RenderRuntime acquisition and no RenderScene. Selecting RenderSystem without a
matching `lux.render.runtime` provider fails closed. This is the dedicated-server boundary; there is no Editor/server
branch and no implicit System injection.

## Preserved holds

Asset residency/demand ownership, ScriptSystem runtime capability injection, product streaming, plugin hot reload and
generic timing/ingress abstractions remain held. This convergence does not authorize Manager/Context/Services/Registry
glue or a second feature/catalog mirror.
