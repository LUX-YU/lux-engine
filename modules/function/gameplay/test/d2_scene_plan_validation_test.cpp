// ============================================================================
//  d2_scene_plan_validation_test.cpp — Gate 0 / D-01: D2ScenePlan::validate()
//  rejects illegal capability combinations wholesale (structured error flags, no
//  assert), accepts legal ones, and is deterministic.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/D2ScenePlan.hpp>
#include <lux/engine/gameplay/2d/Scene2D.hpp>   // platformerPlan

#include <cstdio>

using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== d2_scene_plan_validation_test (D-01) ===\n");

    // ── Legal combinations ──────────────────────────────────────────────────
    check(D2ScenePlan{}.validate().ok(), "an empty plan is valid");
    check(D2ScenePlan{}.enableCore().validate().ok(), "Core alone is valid");
    check(D2ScenePlan{}.enableCore().enableSpriteAnimation().validate().ok(),
          "Core + SpriteAnimation is valid (visual-novel / card shape)");
    check(platformerPlan().validate().ok(),
          "platformerPlan (Core+SpriteAnim+Physics+CharCtrl) is valid");
    check(D2ScenePlan{}.enableCore().enablePixelSimulation()
              .enablePhysics().enablePixelInterop().validate().ok(),
          "Core + PixelSim + Physics + PixelInterop is valid (Noita shape)");

    // ── Missing Core ────────────────────────────────────────────────────────
    {
        const auto v = D2ScenePlan{}.enableSpriteAnimation().validate();
        check(!v.ok() && v.has(D2PlanError::MissingCore),
              "a non-empty plan without Core is rejected (MissingCore)");
    }

    // ── CharacterController without Physics ────────────────────────────────────
    {
        const auto v = D2ScenePlan{}.enableCore().enableCharacterController().validate();
        check(!v.ok() && v.has(D2PlanError::CharacterControllerRequiresPhysics),
              "CharacterController without Physics is rejected");
    }

    // ── PixelInterop missing BOTH sides → reports BOTH errors ──────────────────
    {
        const auto v = D2ScenePlan{}.enableCore().enablePixelInterop().validate();
        check(!v.ok(), "PixelInterop with neither Physics nor PixelSim is invalid");
        check(v.has(D2PlanError::PixelInteropRequiresPhysics), "…reports PixelInteropRequiresPhysics");
        check(v.has(D2PlanError::PixelInteropRequiresPixelSimulation), "…AND PixelInteropRequiresPixelSimulation (all violations at once)");
    }

    // ── PixelInterop with only Physics → still needs PixelSim ───────────────────
    {
        const auto v = D2ScenePlan{}.enableCore().enablePhysics().enablePixelInterop().validate();
        check(!v.ok() && v.has(D2PlanError::PixelInteropRequiresPixelSimulation)
              && !v.has(D2PlanError::PixelInteropRequiresPhysics),
              "PixelInterop + Physics still needs PixelSimulation (only that error)");
    }

    // ── Fixed-step bounds only checked when a fixed-step capability runs ────────
    {
        // A non-simulation plan with a bogus fixed-step is still valid (config unused).
        D2ScenePlan p = D2ScenePlan{}.enableCore().enableSpriteAnimation();
        FixedStepConfig bad; bad.fixed_dt = 0.0f;
        p.setFixedStep(bad);
        check(p.validate().ok(), "a non-simulation plan ignores an unused fixed-step config");

        // With PixelSimulation, the same bogus config is rejected.
        D2ScenePlan q = D2ScenePlan{}.enableCore().enablePixelSimulation();
        q.setFixedStep(bad);
        const auto v = q.validate();
        check(!v.ok() && v.has(D2PlanError::FixedStepInvalidDt),
              "a simulation plan rejects fixed_dt <= 0");
    }
    {
        D2ScenePlan q = D2ScenePlan{}.enableCore().enablePixelSimulation();
        FixedStepConfig bad; bad.max_substeps = 0; bad.max_accumulated = 0.001f;   // < fixed_dt too
        q.setFixedStep(bad);
        const auto v = q.validate();
        check(v.has(D2PlanError::FixedStepInvalidSubsteps), "rejects max_substeps < 1");
        check(v.has(D2PlanError::FixedStepInvalidAccumulated), "rejects max_accumulated < fixed_dt");
    }

    // ── Determinism ────────────────────────────────────────────────────────────
    {
        const D2ScenePlan p = D2ScenePlan{}.enableCore().enablePixelInterop();   // 2 errors
        check(p.validate().errors == p.validate().errors, "validate() is deterministic for the same plan");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
