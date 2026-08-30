# ADR: Spatial Runtime Index Adoption and Streaming Gate

- Status: Needs more evidence
- Date: 2026-08-30
- Inputs: neutral Partition identity, finite Spatial2D/Spatial3D indexes, `process_world`

## Evidence

The concrete 2D and 3D indexes prove immutable finite sparse-grid query semantics over absolute double coordinates.
Queries map point or half-open bounds to caller-owned `PartitionOrdinal` output with deterministic ordering and no hot
path allocation. `process_world` independently proves asynchronous World partition loading and cancellation.

This wave intentionally does not define a Spatial index cooked wire, a World index-root Sender, Scene safe-point
adoption, Registry resource ownership, readiness/generation semantics or the L1-to-L3 decision transport.

## Decision

Runtime Spatial index adoption and both StreamingSystems remain Held. In particular, this review does not authorize a
Registry context/service locator, a generic resource component, SimulationBuilder dependency injection, a streaming
manager, a generic selection DTO or a WorldObjectId-to-Entity map.

A future addendum must first prove exact index wire/load, stale-generation rejection, safe-point adoption and one real
Spatial3D decision-to-load-to-materialize workflow. Spatial2D is the required independent check before any common
streaming contract may be promoted.
