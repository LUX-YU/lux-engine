# ADR-20260830 — Asset Residency Barrier A Review

- Date: 2026-08-30
- Status: Needs more evidence
- Scope: Mesh/Material/Texture demand lifetime promotion
- Input: real preloaded Spatial3D World/Jolt/Render slice

## Evidence

Two materialized World objects reference the same probe-local Mesh, Material and Texture identities. The concrete 3D path
uploads one CPU payload set and creates two Render instances from the same generational GPU handles. Removing the first
instance leaves one live resource-bound instance and the scene remains visible. Removing the final instance reduces the
live instance count to zero; explicit Mesh/Material/Texture destruction then makes the texture slot reusable, and recreation
changes the slot generation from 1 to 2. The full sequence ran through the real Vulkan server with zero validation errors.

This establishes one repeated ownership shape inside Spatial3D:

```text
duplicate concrete interests
  -> one ready resource set
  -> first release preserves readiness
  -> final release makes resources reclaimable
  -> generational handle prevents stale reuse
```

It does not establish AssetId-to-CPU-payload ownership. The inputs were procedural/preloaded, so the evidence does not cover
asynchronous Asset load/decode, failure/retry, cancellation, World generation replacement, cross-scene sharing, or a second
independent product domain.

## Decision

No generic Asset demand/residency API is approved.

The observed duplicate-interest/final-release/generation facts remain concrete Spatial3D evidence. `DemandKey`,
`DemandTracker`, `ResidencyBridge`, `ResourceDemandRegistry`, a manager/token family, and Asset capability injection into
Scene/Simulation remain prohibited. CPU cache policy and Render GPU lifetime remain separate.

Full 3D streaming is blocked at Barrier A. The next admissible evidence is a real AssetId -> cooked load -> CPU decoded
payload -> GPU upload workflow with injected failure/retry and World generation replacement. Only after that evidence, and
preferably an independent 2D/Pixel/Robot consumer, may a new ADR propose a minimal public type budget.

ScriptSystem remains Held; this review does not authorize injecting an Asset service into it.
