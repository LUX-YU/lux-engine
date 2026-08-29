# Phase 10 Evidence Matrix and Barrier B Decision

| Mechanism | 3D | 2D | Pixel | Robot | Same semantics? | Decision |
|---|---:|---:|---:|---:|---|---|
| fixed logical step | no | no | yes | yes | Timing need repeats, ownership evidence is incomplete | keep domain-specific |
| multiple rates | no | no | presentation sample | slow simulation sample | Insufficient | needs more evidence |
| streaming source marker | concrete CPU probe | concrete CPU probe | no | no | Only names/positions overlap | needs more evidence |
| presentation sampling | yes | no | yes | yes | latest-wins compact state | keep existing LatestSpscExchange |
| input ingress | no | no | no | no | No evidence | needs more evidence |
| asset demand lifetime | no | no | no | no | No real Mesh/Material/Texture lifecycle | needs more evidence |
| partition entity bookkeeping | no | no | no | no | No evidence | needs more evidence |

Barrier B creates no new production type. TimeDomainId, TickGroup, phase, ingress, generic streaming marker and
asset-demand APIs remain prohibited. CPU probes validate the existing lane/precision primitives but do not replace the
required Jolt/Render/Asset workloads.
