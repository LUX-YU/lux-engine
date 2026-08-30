# ADR: L1 Domain Partition Identity and Package-Scoped Process Workflows

- Status: Accepted
- Date: 2026-08-30
- Baseline: `5f9f90a9296ba18666c7eb09c895b57a8c8977ca`

## Decision

`DOMAIN` is a narrow target classifier inside L1, not a public architecture layer name. It permits concrete
engine-domain foundations that World and Simulation may share without making either sibling depend on the other.
Generic `domain/common`, `domain/utils` and `domain/services` packages remain forbidden.

Partition build-product identity is owned by `engine/domain/partition`. `PartitionOrdinal` is dense and local to one
build product; `PartitionIndexTypeId` identifies a concrete partition-index representation. Neither type implies that
an index is spatial. World retains durable partition identity, storage tables, index descriptors and opaque build
artifacts. Spatial indexes depend on Partition and map absolute space to ordinals.

The existing Script description also needs durable authored object identity without depending on World. The narrow
`engine/domain/world_identity` leaf therefore owns `WorldObjectId` and its comparison/hash values. This is identity only;
it owns no World description, storage, resolver or runtime object mapping.

Simulation may depend on DOMAIN foundations and must not depend on World. World may depend on DOMAIN foundations and
must not depend on Simulation.

Process is package-scoped. `process/execution` remains domain-blind. `process/world` and `process/asset` may own typed,
time-spanning workflows over lower-layer contracts, but may not depend on Scene, Render, Editor, Toolchain or gameplay
policy. This does not authorize ProcessRuntime, AsyncRuntime, a service bag, a workflow registry or a manager.

## Held

CPU Asset lifecycle/deduplication, runtime Spatial index adoption, Spatial StreamingSystems, index cooked wire and Asset
residency remain unapproved. They require later evidence and a separate decision-complete addendum.
