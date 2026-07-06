// ============================================================================
//  d2_scene_plan_validation_test.cpp — Gate 0 / D-01: D2ScenePlan::validate()
//  rejects illegal capability combinations wholesale (structured error flags, no
//  assert), accepts legal ones, and is deterministic.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/D2ScenePlan.hpp>
#include <lux/engine/gameplay/2d/Scene2D.hpp>   // traditional2DPlan

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
    check(D2ScenePlan{}.enableCore().enableSpriteRendering().validate().ok(),
          "Core + SpriteRendering is valid (traditional 2D)");
    check(D2ScenePlan{}.enableCore().enableSpriteRendering().enableSpriteAnimation().validate().ok(),
          "Core + SpriteRendering + SpriteAnimation is valid");
    check(traditional2DPlan().validate().ok(),
          "traditional2DPlan (Core + SpriteRendering) is valid");
    check(D2ScenePlan{}.enableCore().enablePixelSimulation()
              .enablePhysics().enablePixelInterop().validate().ok(),
          "Core + PixelSim + Physics + PixelInterop is valid (Noita shape)");

    // ── Missing Core ────────────────────────────────────────────────────────
    {
        const auto v = D2ScenePlan{}.enableSpriteAnimation().validate();
        check(!v.ok() && v.has(D2PlanError::MissingCore),
              "a non-empty plan without Core is rejected (MissingCore)");
    }

    // ── Sprite capability dependencies ─────────────────────────────────────────
    {
        const auto v = D2ScenePlan{}.enableSpriteRendering().validate();
        check(!v.ok() && v.has(D2PlanError::SpriteRenderingRequiresCore),
              "SpriteRendering without Core is rejected (needs Transform2D + Camera2D)");
    }
    {
        const auto v = D2ScenePlan{}.enableCore().enableSpriteAnimation().validate();
        check(!v.ok() && v.has(D2PlanError::SpriteAnimationRequiresSpriteRendering),
              "SpriteAnimation without SpriteRendering is rejected (animates a rendered sprite)");
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
        D2ScenePlan p = D2ScenePlan{}.enableCore().enableSpriteRendering().enableSpriteAnimation();
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
    // validate() is a pure function of the capability SET (a bitmask read in a fixed
    // order), not of the order capabilities were enabled or of plan identity. Testing
    // that requires two DIFFERENTLY-built plans — comparing one plan's result to itself
    // would be a tautology (a pure const method returns the same value by definition).
    {
        // Same set (Core + PixelInterop, both seam sides missing → 2 errors), enabled
        // in OPPOSITE orders: order must not change a single error bit.
        const auto a = D2ScenePlan{}.enableCore().enablePixelInterop().validate();
        const auto b = D2ScenePlan{}.enablePixelInterop().enableCore().validate();
        check(a.errors == b.errors,
              "validate() depends only on the capability set, not the enable order");
        check(!a.ok()
              && a.has(D2PlanError::PixelInteropRequiresPhysics)
              && a.has(D2PlanError::PixelInteropRequiresPixelSimulation),
              "…and the shared result pins BOTH PixelInterop violations (exact bits)");

        // A copy is an independent value that validates identically (no shared state).
        const D2ScenePlan original = D2ScenePlan{}.enableCore().enablePhysics().enablePixelInterop();
        const D2ScenePlan copy = original;
        check(original.validate().errors == copy.validate().errors,
              "a copied plan validates identically (pure value type)");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
