# ADR: L1–L3 Retained 3D Render Lane

- Status: Accepted
- Date: 2026-08-31
- Scope: Mesh3D and Light3D only

## Decision

Simulation owns authoritative `Mesh3D`, `Light3D` and double-precision `WorldTransform3D` facts. At the stable point,
the L3 `RenderSystem` coalesces dirty entity generations into bounded `RenderProgram(StateUpdate)` packets. Presentation
forwards optional updates and authors `RenderProgram(Frame)` packets. The Render thread applies state updates to retained
`RenderScene` entities without advancing the frame lifecycle; only Frame programs render and present.

Cross-lane scene identity is `(RenderSceneId, RenderEntityId)`. `RenderObjectHandle` and `RLightHandle` remain private
Render implementation identities. Mesh and material uploads register explicit `AssetId` values; a mesh upsert referencing
a missing or still-uploading asset fails closed and creates no pending state.

`LatestSpscExchange<T>` remains the approved latest-wins sampling primitive for Pixel, Robot, Spatial2D and other compact
presentation states. It is not used to copy Registry/Scene state and is not retired by this 3D lane.

## Rejected expansion

This decision does not authorize a Presentation Registry, full-scene snapshot, generic delta, Asset resolver, residency
manager, timing domain, streaming marker or multi-task System API. Canvas and the other held Render protocols retain their
existing reply/identity contracts.
