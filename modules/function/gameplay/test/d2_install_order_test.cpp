// ============================================================================
//  d2_install_order_test.cpp — Gate 0 / D-02: d2::install() adds systems in ONE
//  deterministic pass — exactly ONE Simulation2DSystem for any fixed-step plan
//  (single accumulator, never doubled), Transform2DSystem for a Core plan, none for
//  an invalid plan or a PixelSimulation plan given a null runtime (no partial install).
//
//  A valid fixed-step plan is always also a Core plan (MissingCore rule), so it
//  installs the Simulation2DSystem + Core's Transform2DSystem + Camera2DSystem (3
//  systems); a double-installed accumulator would push that to 4, so systemCount == 3
//  pins "one accumulator". (SpriteAnim arrives later in Slice A and will grow these.)
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

    // A non-null PixelFieldRuntime* sentinel — install() only null-checks it (never
    // dereferences at this stage), so an incomplete-type pointer is fine for the test.
    PixelFieldRuntime* fakeRuntime()
    {
        static int sentinel = 0;
        return reinterpret_cast<PixelFieldRuntime*>(&sentinel);
    }
}

int main()
{
    std::printf("=== d2_install_order_test (D-02) ===\n");

    // Physics-only (Core + Physics) → Simulation2D + Transform2D + Camera2D (one accumulator).
    {
        World w;
        const auto r = install(w, /*runtime=*/nullptr, D2ScenePlan{}.enableCore().enablePhysics());
        check(w.systemCount() == 3 && r.simulation && r.transform && r.camera,
              "physics-only installs Simulation2D + Transform2D + Camera2D");
    }

    // Pixel-only (with a runtime) → Simulation2D + Transform2D + Camera2D.
    {
        World w;
        const auto r = install(w, fakeRuntime(), D2ScenePlan{}.enableCore().enablePixelSimulation());
        check(w.systemCount() == 3 && r.simulation && r.transform && r.camera,
              "pixel-only installs Simulation2D + Transform2D + Camera2D");
    }

    // Pixel + Physics + Interop → still exactly ONE Simulation2DSystem (single accumulator).
    {
        World w;
        const auto r = install(w, fakeRuntime(),
            D2ScenePlan{}.enableCore().enablePhysics().enablePixelSimulation().enablePixelInterop());
        check(w.systemCount() == 3 && r.simulation,
              "pixel+physics+interop installs ONE Simulation2DSystem (single accumulator, count 3 not 4)");
    }

    // Sprite-only (Core + SpriteRendering) → Transform2D + Camera2D, no Simulation2DSystem
    // (SpriteRendering is a render bridge, not a World system).
    {
        World w;
        const auto r = install(w, nullptr, D2ScenePlan{}.enableCore().enableSpriteRendering());
        check(w.systemCount() == 2 && r.simulation == nullptr && r.transform && r.camera,
              "sprite-only installs Core (Transform2D + Camera2D) but no Simulation2DSystem");
    }

    // Invalid plan (CharacterController without Physics) → nothing installed.
    {
        World w;
        const auto r = install(w, nullptr, D2ScenePlan{}.enableCore().enableCharacterController());
        check(w.systemCount() == 0 && r.simulation == nullptr && r.transform == nullptr,
              "an invalid plan installs nothing (no partial install)");
    }

    // PixelSimulation plan given a null runtime → nothing installed.
    {
        World w;
        const auto r = install(w, /*runtime=*/nullptr, D2ScenePlan{}.enableCore().enablePixelSimulation());
        check(w.systemCount() == 0 && r.simulation == nullptr,
              "a PixelSimulation plan with a null runtime installs nothing");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
