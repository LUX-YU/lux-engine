# ADR: L1–L3 Retained 3D Render Lane

- Status: Accepted
- Date: 2026-08-31
- Scope: Mesh3D and Light3D only

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

`LatestSpscExchange<T>` remains the approved latest-wins sampling primitive for Pixel, Robot, Spatial2D and other compact
presentation states. It is not used to copy Registry/Scene state and is not retired by this 3D lane.

## Rejected expansion

This decision does not authorize a Presentation Registry, full-scene snapshot, generic delta, Asset resolver, residency
manager, timing domain, streaming marker or multi-task System API. Canvas and the other held Render protocols retain their
existing reply/identity contracts.
