// ============================================================================
//  simulation_phase_order_test.cpp — Gate 0 / D-03: Simulation2DSystem runs its
//  wired phases in the canonical single-direction order each substep, and skips
//  unwired phases (so subsets run in the same relative order).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/world/systems/Simulation2DSystem.hpp>
#include <lux/engine/gameplay/2d/D2ScenePlan.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>
#include <vector>

using namespace lux::gameplay::d2;
using Phase = Simulation2DSystem::Phase;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    // Wire a phase to record its index when it runs.
    void record(Simulation2DSystem& sim, std::vector<int>& trace, Phase p)
    {
        const int idx = static_cast<int>(p);
        sim.setPhase(p, [&trace, idx](lux::meta::EntityRegistry&, float) { trace.push_back(idx); });
    }
}

int main()
{
    std::printf("=== simulation_phase_order_test (D-03) ===\n");

    lux::meta::EntityRegistry reg;
    FixedStepConfig cfg;

    // ── All phases wired → one substep runs them in canonical enum order ──
    {
        Simulation2DSystem sim(cfg);
        std::vector<int> trace;
        for (int i = 0; i < static_cast<int>(Simulation2DSystem::kPhaseCount); ++i)
            record(sim, trace, static_cast<Phase>(i));

        sim.update(reg, cfg.fixed_dt);   // exactly one substep

        bool ordered = (trace.size() == Simulation2DSystem::kPhaseCount);
        for (int i = 0; ordered && i < static_cast<int>(trace.size()); ++i)
            ordered = (trace[i] == i);
        check(ordered, "one substep runs all phases in canonical order (ApplyFieldCommands..PublishEvents)");
    }

    // ── A subset (Physics-only) runs just that phase; unwired phases are skipped ──
    {
        Simulation2DSystem sim(cfg);
        std::vector<int> trace;
        record(sim, trace, Phase::SimulatePhysics);

        sim.update(reg, cfg.fixed_dt);
        check(trace.size() == 1 && trace[0] == static_cast<int>(Phase::SimulatePhysics),
              "an unwired phase is skipped (only the wired one runs)");
    }

    // ── Two substeps run the full ordered pipeline twice ──
    {
        Simulation2DSystem sim(cfg);
        std::vector<int> trace;
        record(sim, trace, Phase::ApplyFieldCommands);
        record(sim, trace, Phase::PublishEvents);

        sim.update(reg, 2.0f * cfg.fixed_dt);   // two substeps
        check(sim.substepsLastFrame() == 2, "2*fixed_dt → two substeps");
        check(trace.size() == 4
              && trace[0] == static_cast<int>(Phase::ApplyFieldCommands)
              && trace[1] == static_cast<int>(Phase::PublishEvents)
              && trace[2] == static_cast<int>(Phase::ApplyFieldCommands)
              && trace[3] == static_cast<int>(Phase::PublishEvents),
              "each substep re-runs the wired phases in order");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
