# Script Hook migration inventory — 2026-09-05

Implementation candidate: `5f03e9b156421583ae81857025ec6156ad0e0f05`.

## Shipped owner inventory

The production `engine/` source (excluding tests/benchmarks) contains the generic Hook declaration/bridge, but no
concrete builtin that dispatches a Script-capable Hook in the middle of its algorithm. Physics2D has its real
`execute`/step and `overlapsBox` state; it does not claim a production Collision/Trigger event protocol. Transform
propagation remains a normal System task. No unobserved domain callback was renamed or assigned a guessed timestep.

| Consumer / declaration | Old invocation | Parameters | Current compiled placement | Later dependency |
|---|---|---|---|---|
| Scene Script runtime probe | primary task / Scene pump | `void()` | producer task → caller stable Hook | final derived-data task → Scene publication |
| Scene Lua runtime probe | primary task dispatch | `void()` | producer task → caller stable Hook | Scene work; no Scene gameplay pump |
| Physics Script probe | primary task + manual delivery | `void()`; scalar pulse | Physics task and pulse producer → caller stable Hook | Simulation completion |
| FlowForge AOT runtime/benchmark probe | primary Hook and manual Event drain | `void()`; owned i32 payload | producer → compiled stable Hook / Channel | normal bounded resume path |
| Installed C++ coroutine consumer | manual Hook/Event/resume | `void()`; i32 pulse | real Physics + producer → stable Hook | Event → NextStep → Entity reuse → teardown |
| Installed Hook binding consumer | manual calls | explicit `float` value, copied collision fixture | producer → value Hook → Event/stable Hook | no inferred `float dt` semantics |
| Installed Event runtime consumer | manual Event drain/resume | owned i32 | producer → compiled Event/stable Hook | dispatch observation before stable resume |
| Installed Inventory Lua consumer | package/backend construction only | `void()`; i32 Event | owned Inventory System → stable Hook | eager Ability → Event → same Lua object → EndPlay |
| Multi-region qualification System | new proof fixture | `void()`; owned i32 lanes | produce → first Hook/commit → middle → stable Hook/commit → propagate | real Registry observer follow-up batches |

Standalone primitive/runtime micro tests use non-installed `HookInvocationTestAccess` / `ScriptEndpointTestAccess`.
They do not install gameplay composition or prove production dispatch. The Hook test accessor rejects a composed
endpoint. Production endpoints require a private graph-issued invocation; Channel Script consumption is additionally
authorized only during the compiled delivery interval and rejects reentry/early descriptor consumption.

## Transport / command contracts

- `HookChannel` owns no subscriber registry, mutex, atomic per-record queue, scheduler or coroutine state.
- Simulation owns storage; Systems borrow typed channels and prepared producer ports. Stable System/stage identity
  assigns lane order. Port activation is confined to its compiled node.
- Native consumers borrow sealed lane spans. Script owns one endpoint-level bridge and copies into the existing
  owned resume representation before READY. Non-scalar channel payloads require an explicit ownership copy.
- Overflow fails the step and skips dependent work. Cleanup/reset still runs. Re-production is opt-in and uses
  next-generation storage, not the current sealed batch.
- Structural producer ports are point-owned. Commits snapshot all admitted records once; observer-generated
  records cannot feed the same batch. The final scripted commit's follow-ups wait for the next step, after—not
  behind—the current step's final derived propagation. Non-scripted Simulation retains its normal final commit.
- Record/admission failure is fail-closed; foreign Registry/component application failure does not roll back an
  already applied prefix. No global ECS destruction ordering or transaction framework was introduced.

## Explicit limitations

Collision/Trigger/death-style data in transport tests is synthetic, not a newly published Physics contact API.
This wave demonstrates real Physics2D step/query integration and explicit multi-region execution, not a new gameplay
framework. Capacities are fixed while running; no automatic growth/shrink, VM coroutine pooling, arbitrary live
owner migration, second Scene TaskGraph or new scheduler exists. R1 is not started.
