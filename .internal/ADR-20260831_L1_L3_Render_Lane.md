# ADR: L1–L3 Retained 3D Render Lane

- Status: Accepted
- Date: 2026-08-31
- Scope: Mesh3D and Light3D only
- Extraction foundation: Closed by `0d2d6e8fbdcb08f5bc5b82c0a928f45f8b06b806`

## Decision

Simulation owns authoritative `Mesh3D`, `Light3D` and double-precision `WorldTransform3D` facts. Mesh and Light each own a
private L3 `RenderSyncStage`, reactive membership, published state and feature-specific diff. The generic `RenderSystem`
only prepares those stages in stable order, publishes one bounded `RenderProgram(StateUpdate)` transaction and then
commits or discards every stage together. Presentation forwards optional updates and authors `RenderProgram(Frame)`
packets. The Render thread applies state updates to retained `RenderScene` entities without advancing the frame lifecycle;
only Frame programs render and present.

Cross-lane scene identity is `(RenderSceneId, RenderEntityId)`. `RenderObjectHandle` and `RLightHandle` remain private
Render implementation identities. Pure Render upload requests return `RMeshHandle`/`RMaterialHandle`, and mesh upserts
carry only those handles. L3 owns `ResolvedMeshResources`, which binds authored Asset intent to ready handles; a stage
publishes a resource switch only when the resolved source AssetIds still match the current `Mesh3D` description. Pure
Render owns no AssetId map or Asset lookup.

EnTT reactive storage is the active change foundation. Each stage connects signals and folds existing matching entities at
attach time. Backpressure and encode failures retain reactive/departure state; published state changes only after the
StateUpdate packet is accepted, or after a prepared no-command transaction is explicitly committed.

Prepare owns every fallible allocation. A first renderable entity receives a private unpublished state slot during
prepare; discard retains that slot for retry, while departure removes it without a wire Remove. Commit only assigns an
already-existing nothrow state, removes a state under departure suppression, and clears retained containers. Full sync is
requested centrally by RenderSystem and traverses the current feature view during prepare; its noexcept request path only
sets flags. Departure callbacks append in amortized O(1), and prepare performs sort/unique before encoding removals.

Mesh departure cancellation uses the same currently-renderable predicate as ordinary extraction: component membership,
authored/resolved AssetId agreement and non-null Render resource handles must all hold. Reintroducing stale or mismatched
resolved data therefore cannot preserve an obsolete retained Render instance.

`LatestSpscExchange<T>` remains the approved latest-wins sampling primitive for Pixel, Robot, Spatial2D and other compact
presentation states. It is not used to copy Registry/Scene state and is not retired by this 3D lane.

## Rejected expansion

This decision does not authorize a Presentation Registry, full-scene snapshot, generic delta, Asset resolver, residency
manager, timing domain, streaming marker or multi-task System API. Canvas and the other held Render protocols retain their
existing reply/identity contracts.

The later SceneSystem convergence may replace nullable Registry pointers in builtin stage factories and allow an empty
RenderSystem StageList. Those public API decisions are deliberately outside this closure.
