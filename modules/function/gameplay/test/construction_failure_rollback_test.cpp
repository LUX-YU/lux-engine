// ============================================================================
//  construction_failure_rollback_test.cpp — Gate 0 / D-04: the 2D scene BRING-UP
//  is an all-or-nothing transaction, tested against the real d2::install().
//
//  install() either wires a plan in full or wires NOTHING — it never leaves a
//  partially-installed scene behind (README "Scene lifecycle contract"). A plan it
//  refuses — dependency-invalid, or PixelSimulation without a live runtime — adds
//  ZERO systems. The property no other test pins: a refused bring-up leaves NO
//  RESIDUE, so a later valid install() on the SAME World still produces exactly its
//  own systems (the failed attempt cannot corrupt a subsequent one).
//
//  (d2_install_order_test covers refusal on a *fresh* World; this covers refusal
//  then RECOVERY on the same World — the rollback-cleanliness half.) This drives real
//  engine code, not a mock: the earlier RAII stand-in only re-proved that C++ locals
//  unwind in reverse — a language guarantee, not an engine invariant.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>          // install / D2Installed / D2ScenePlan
#include <lux/engine/gameplay/world/World.hpp>          // systemCount()

#include <cstdio>

using lux::gameplay::World;
using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== construction_failure_rollback_test (D-04) ===\n");

    World world;

    // ── A dependency-invalid plan is refused wholesale — nothing wired ──────────
    // Core + CharacterController without Physics violates CharacterControllerRequiresPhysics,
    // so validate() fails and install() must add no systems and return an empty handle set.
    {
        const auto r = install(world, /*runtime=*/nullptr,
                               D2ScenePlan{}.enableCore().enableCharacterController());
        check(world.systemCount() == 0
              && r.simulation == nullptr && r.transform == nullptr && r.camera == nullptr,
              "an invalid plan installs nothing (no partially-wired scene)");
    }

    // ── A valid plan missing its runtime is refused at install time — still nothing ─
    // Core + PixelSimulation validates, but PixelSimulation needs a live runtime; with a
    // null runtime install() refuses it. The World is still untouched by the FIRST refusal.
    {
        const auto r = install(world, /*runtime=*/nullptr,
                               D2ScenePlan{}.enableCore().enablePixelSimulation());
        check(world.systemCount() == 0 && r.simulation == nullptr,
              "a valid plan without its required runtime installs nothing");
    }

    // ── Recovery: a refused bring-up left no residue ────────────────────────────
    // After TWO refused installs on this same World, a valid install must produce EXACTLY
    // its own systems (traditional 2D = Transform2D + Camera2D). systemCount() == 2 proves
    // neither refusal leaked a half-built system into the World.
    {
        const auto r = install(world, /*runtime=*/nullptr, traditional2DPlan());
        check(world.systemCount() == 2
              && r.transform != nullptr && r.camera != nullptr && r.simulation == nullptr,
              "a valid install after refusals wires exactly its own systems (no residue)");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
