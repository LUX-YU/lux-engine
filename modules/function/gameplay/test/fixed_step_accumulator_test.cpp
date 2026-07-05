// ============================================================================
//  fixed_step_accumulator_test.cpp — Gate 0 / D-03: Simulation2DSystem's fixed-step
//  accumulator produces a predictable substep count for a given dt, clamps banked
//  time + caps substeps/frame (discarding + recording the backlog), runs a
//  physics-only scene (no field gating), and no-ops when no phase is wired.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/world/systems/Simulation2DSystem.hpp>
#include <lux/engine/gameplay/2d/D2ScenePlan.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>

using namespace lux::gameplay::d2;
using Phase = Simulation2DSystem::Phase;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== fixed_step_accumulator_test (D-03) ===\n");

    lux::meta::EntityRegistry reg;
    FixedStepConfig cfg;   // fixed_dt = 1/60, max_substeps = 4, max_accumulated = 0.25
    const float fdt = cfg.fixed_dt;

    // No phase wired → the coordinator is a zero-cost no-op regardless of dt.
    {
        Simulation2DSystem sim(cfg);
        sim.update(reg, 1.0f);
        check(sim.substepsLastFrame() == 0 && sim.totalSubsteps() == 0,
              "no phase wired → update() is a no-op (never runs on empty)");
    }

    // Physics-only: a single SimulatePhysics phase steps even with NO field entities.
    Simulation2DSystem sim(cfg);
    int calls = 0;
    sim.setPhase(Phase::SimulatePhysics, [&](lux::meta::EntityRegistry&, float) { ++calls; });

    sim.update(reg, fdt);
    check(sim.substepsLastFrame() == 1 && calls == 1, "dt == fixed_dt → exactly 1 substep (physics-only, no fields)");

    sim.update(reg, fdt);
    check(sim.substepsLastFrame() == 1 && calls == 2, "a second fixed_dt → 1 more substep");

    sim.update(reg, 2.5f * fdt);
    check(sim.substepsLastFrame() == 2 && calls == 4, "dt == 2.5 fixed_dt → 2 substeps, 0.5 banked");

    // A huge dt: banked time is CLAMPED to max_accumulated, then substeps are CAPPED at
    // max_substeps, and the leftover backlog is DISCARDED (and recorded as lag).
    const float lag_before = sim.laggedTime();
    sim.update(reg, 1.0f);
    check(sim.substepsLastFrame() == cfg.max_substeps, "a huge dt is capped at max_substeps");
    check(calls == 4 + cfg.max_substeps, "the capped frame ran exactly max_substeps phase calls");
    check(sim.laggedTime() > lag_before, "hitting the cap with time banked records dropped lag");
    check(sim.accumulated() < fdt, "after a capped+dropped frame less than one step remains banked");

    check(sim.totalSubsteps() == static_cast<std::uint64_t>(1 + 1 + 2 + cfg.max_substeps),
          "totalSubsteps accumulates across frames");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
