// ============================================================================
//  2d_empty_kit_compile_test.cpp — Gate 0 / D-00: the 2D kit (lux::gameplay::d2)
//  compiles + links, its D2ScenePlan capability builder works, and installing an
//  EMPTY plan onto a World installs NO systems (payment symmetry — a scene that
//  wants nothing pays nothing).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/world/World.hpp>

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
    std::printf("=== 2d_empty_kit_compile_test (D-00) ===\n");

    // 1. A default plan is empty and needs no fixed-step coordinator.
    D2ScenePlan empty;
    check(empty.empty(), "a default D2ScenePlan is empty");
    check(!empty.needsSimulation(), "an empty plan needs no Simulation2DSystem");

    // 2. The capability builder sets exactly the requested flags.
    const D2ScenePlan p = D2ScenePlan{}.enableCore().enablePixelSimulation();
    check(p.has(D2Capability::Core), "enableCore() sets Core");
    check(p.has(D2Capability::PixelSimulation), "enablePixelSimulation() sets PixelSimulation");
    check(!p.has(D2Capability::Physics), "an un-enabled capability stays off");
    check(p.needsSimulation(), "a pixel-sim plan needs a Simulation2DSystem");

    // 3. The traditional-2D preset enables exactly the installable capability set, and
    //    validates (a preset must not promise a capability install would silently drop).
    const D2ScenePlan trad = traditional2DPlan();
    check(trad.has(D2Capability::Core) && trad.has(D2Capability::SpriteRendering),
          "traditional2DPlan() = Core + SpriteRendering");
    check(!trad.has(D2Capability::Physics) && !trad.has(D2Capability::PixelSimulation),
          "traditional2DPlan() promises no unimplemented capability");
    check(trad.validate().ok(), "traditional2DPlan() validates");

    // 4. Installing an EMPTY plan installs NO systems (the D-00 contract).
    {
        World world;
        install(world, /*runtime=*/nullptr, D2ScenePlan{});
        check(world.systemCount() == 0, "install(empty plan) installs no systems");
        world.tick(1.0f / 60.0f);   // no systems → clean no-op, must not crash
        check(true, "tick() on an empty 2D world runs cleanly");
    }

    // 5. A configured fixed-step is carried on the plan.
    D2ScenePlan fs;
    FixedStepConfig cfg;
    cfg.fixed_dt = 1.0f / 50.0f;
    fs.setFixedStep(cfg);
    check(fs.fixedStep().fixed_dt == 1.0f / 50.0f, "setFixedStep is carried by the plan");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
