# ADR-20260830 — Simulation One-Primary-Task Evidence Review

- Date: 2026-08-30
- Status: Accepted
- Scope: L1 Simulation scheduling surface
- Inputs: Transform hierarchy maintenance gap and real Jolt 5.5.0 double-position probe

## Context

The frozen Simulation v1 contract permits each concrete System to contribute zero or one primary Task. Transform is the
first counterexample: hierarchy maintenance must precede Transform2D/3D delta consumption, and registering both Transform
Systems naively would duplicate or race that maintenance. One counterexample does not establish a generic multi-node System
contract, so the scheduling surface was held pending an independent real workload.

The independent workload is `architecture_probe_jolt_l1`. It installs a concrete `JoltProbeSystem` through the actual
`SystemRegistration` and `SimulationBuilder`, and its single primary Task calls `JPH::PhysicsSystem::Update()` with a
probe-local fixed step. Pre-step body operations, the solver, contacts and queries are naturally contained by that one Task;
the probe did not need pre-physics/post-physics TaskGraph nodes.

## Decision

Keep the one-primary-task public contract.

Jolt does not provide a second independent multi-node requirement. Transform remains a concrete/lower-domain composition
problem: its hierarchy-maintenance ownership must be reorganized without widening `SimulationBuilder`. This ADR does not
authorize public `TaskGroup`, a private multi-task table, a completion fence, an execution-point graph, or any new scheduler
type. If a future independent domain demonstrates the same multi-node ownership need, a new addendum must repeat this
evidence review before changing the Builder.

ScriptSystem remains Held. No Asset/backend capability bag is added to Simulation or Scene.

## Evidence

- Jolt overlay and bootstrap ABI contract: `a57c75e3`.
- Real Jolt L1 probe: `49404081acb1e18c2efacd442a8d07e94d701e87`.
- CTest: 109/109 passed; `architecture_probe_jolt_l1` is a HOST/TEST/COMPOSITION product.
- Qualification: 10,000 dynamic + 100,000 static bodies at near origin and at `1e12`, with one primary Task and three
  explicitly bounded Jolt workers.
- JPH types remain confined to the probe translation unit; an architecture gate rejects Jolt includes/names in installed
  public headers.

## Consequences

The follow-up Transform ownership review resolved the held lower-domain gap without a scheduling addendum. One
package-private registered Transform composition owns `HierarchyIndex`, `HierarchyDeltaBatch` and
`HierarchyMaintenance`; its single command primary task executes maintenance exactly once, then the existing 2D and 3D
propagation mechanisms against the same delta batch. Any step failure discards the shared command producer. Public
`SimulationBuilder` remains unchanged and the one-primary-task decision stands.

Generic timing, phase, ingress, streaming and demand types remain prohibited.
