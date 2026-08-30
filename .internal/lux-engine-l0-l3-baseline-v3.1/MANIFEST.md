# MANIFEST — Lux Engine L0–L3 Architecture Baseline v3.1

Target repository baseline: `LUX-YU/lux-engine@230374a5f0d53e52bbb5d3bdce33cac62da06660`.

This repository-canonical bundle supersedes the previous v3 implementation-spec package for construction purposes and
includes the approved corrections documented in `REPOSITORY-ERRATA.md`.

## Ordered contents

| File | Lines | SHA-256 | Role |
|---|---:|---|---|
| `00-l0-l3-master-implementation-plan.zh-CN.md` | 1319 | `9958e7842f4cf557d7f925499e73d36e341d57b25926a304a350b7c068891221` | Normative master construction DAG / type budgets / barriers |
| `01-l0-taskgraph-dependency-list-prerequisite.zh-CN.md` | 232 | `bd2868aedd1482b274be91940e64fe6c9a7d6f84c3fec3ca5f5020e79fa14ffd` | Phase 1 — L0 TaskDependencies prerequisite |
| `02-world-description-v2-storage-implementation-spec.zh-CN.md` | 723 | `199ff60fdbfa761d048a7bc30ab5d4c5034d5c2f116ff197890fa3a522539d3c` | Phases 2–3 — World semantic + physical storage |
| `03-ecs-double-precision-component-decode-prerequisite.zh-CN.md` | 594 | `4f2f16b86270b2007178356ef5bd553f9679226b8ff98dbf367e4bfd101841b8` | Phase 4 — canonical double + generated decode/emplace |
| `04-simulation-system-registry-runtime-implementation-spec.zh-CN.md` | 909 | `4097832bf259818cf365bb3ff76fc9f0e16f758fc747f1abf7f843e0c4f9aea3` | Phase 5 — System type catalog + Simulation runtime |
| `05-scene-core-description-runtime-composition-spec.zh-CN.md` | 489 | `ea45ceb353425a8493ea4738ae9b320c4cfd4c488258aa7af3ba0ebaabb59f61` | Phase 7 — SceneDescription + minimal Scene |
| `06-scene-runtime-world-process-materialization-spec.zh-CN.md` | 731 | `c2f27f07ddb1293d98ee8827fe1a844b7924ae2b1777974391f4bce38c7b84ee` | Phase 8 — WorldStorageSource/load/materialize mechanism |
| `07-system-luxobject-streaming-resource-protocol.zh-CN.md` | 478 | `fd1453cd45b8fa883abb7db7b7ab1fddd334488c4368e7327c8c91ded05d2023` | Cross-phase concrete System/LuxObject/streaming constraints |
| `08-engine-asset-residency-design-hold.zh-CN.md` | 212 | `e565ae24686c17caa843da2a75540e155be7f88c799a224552d0c20d39dbc26a` | Design Barrier A — ownership frozen, generic demand API held |
| `09-runtime-execution-lanes-presentation-render-contract.zh-CN.md` | 621 | `110eeb88ba7c593934f79846283a9a31f5df3d9b668e97549a1dfd130888453b` | Phase 9 — latest-wins SPSC state exchange + lane contract |
| `10-topology-cmake-architecture-gates.zh-CN.md` | 512 | `921bfa2ceeb5642766290359d9b98f816862e8325a17fe949bd5c8ca22781e35` | CMake/target/package architecture gates |
| `11-architecture-probes-3d-2d-pixel-robot.zh-CN.md` | 536 | `4951e7dc0e3833961669245d13ff18423f07b43c972384f10df6c0ff1725b03f` | Phase 10 — four product architecture probes |
| `12-p1-backlog.zh-CN.md` | 39 | `8b70a3815c371c87eafc90dc47676b55d59355b195f90b5294b94e641ce41d5f` | P1 backlog after Barrier B |
| `ARCHITECTURE-GAPS.md` | 14 | `92a2f3f0f06ecad655616125bb45a3ee159a5120721c52c9051e5ab2dab56517` | Active architecture gap log |
| `README.md` | 254 | `fcb307a962913524294e5577450523b915201452cbfb6db14af9b4082f4665cb` | Package entry point / ordered reading guide |
| `REPOSITORY-ERRATA.md` | 27 | `8dced54d37140c52bb66891ba72a5092fbef3068de7f9ecbb07fa3380e627e9c` | Approved repository-specific corrections |

## Construction phases

```text
0  SSOT / supersede / soft gates
1  L0 TaskDependencies prerequisite
2  L1 World semantic metadata
3  L1 World physical storage/wire
4  L1 ECS double precision + generated component decode/emplace
5  L1 Simulation runtime/SystemRegistry type catalog
6  L2 Process verification (tests-only by default)
7  L3 Scene core
8  L3 scene/runtime/world
A  Design Barrier A — Asset residency demand contract
9  L3 LatestSpscExchange<T>
10 Product architecture probes: 3D / 2D / Pixel / Robot
B  Design Barrier B — phase/time/common runtime promotion
11 Hard architecture gates
```

## Public-surface discipline

- A phase may add only the public production types explicitly listed in its specification.
- Missing prerequisite capability is fixed in the owning lower layer; do not insert adapters/contexts/managers to bridge it.
- `TimeDomainId`, `TickGroup`, `ScenePhase`, generic streaming-source types, and generic asset-demand wiring are prohibited before their design barriers.
- Private `Impl`, wire structs, helper functions, test fixtures, and paired failure types do not consume the public type budget.
