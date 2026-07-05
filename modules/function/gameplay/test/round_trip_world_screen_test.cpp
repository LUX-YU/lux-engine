// ============================================================================
//  round_trip_world_screen_test.cpp — Slice A / T2-02: the Camera2D world↔screen
//  helpers are exact inverses (screenToWorld ∘ worldToScreen == identity) for a
//  moved / rotated / y-flipped / non-square-aspect camera, and the camera's own
//  position maps to the screen centre.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/world/systems/Camera2DSystem.hpp>   // worldToScreen / screenToWorld
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DCacheComponent.hpp>
#include <lux/engine/gameplay/world/World.hpp>

#include <Eigen/Core>
#include <cstdio>

using lux::gameplay::World;
using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    void roundTrips(const Camera2DCacheComponent& cache, const Eigen::Vector2f& vp, const char* label)
    {
        const Eigen::Vector2f pts[] = {
            {5.f, 3.f}, {0.f, 0.f}, {15.f, 8.f}, {-4.f, 9.f}, {5.f + 10.f, 3.f}
        };
        bool ok = true;
        for (const auto& p : pts)
        {
            const Eigen::Vector2f s    = worldToScreen(cache, vp, p);
            const Eigen::Vector2f back = screenToWorld(cache, vp, s);
            ok = ok && (back - p).norm() < 1e-2f;
        }
        check(ok, label);
    }
}

int main()
{
    std::printf("=== round_trip_world_screen_test (T2-02) ===\n");

    World world;
    install(world, /*runtime=*/nullptr, D2ScenePlan{}.enableCore());

    const auto cam = world.createEntity();
    auto& tc = world.emplace<Transform2DComponent>(cam);
    tc.position = Eigen::Vector2f(5.f, 3.f);
    auto& cc = world.emplace<Camera2DComponent>(cam);
    cc.units_per_view_height = 10.f;
    cc.aspect = 2.f;
    const Eigen::Vector2f vp(800.f, 400.f);

    world.tick(1.f / 60.f);
    {
        const auto& cache = world.get<Camera2DCacheComponent>(cam);
        roundTrips(cache, vp, "world→screen→world round-trips (moved camera, aspect 2)");
        const Eigen::Vector2f centre = worldToScreen(cache, vp, Eigen::Vector2f(5.f, 3.f));
        check((centre - vp * 0.5f).norm() < 0.5f, "the camera's world position maps to the screen centre");
    }

    // Rotated + y-flipped camera — the helpers still round-trip (shared convention).
    tc.rotation = 0.5f; tc.dirty = true;
    cc.y_flip = true;
    world.tick(1.f / 60.f);
    roundTrips(world.get<Camera2DCacheComponent>(cam), vp,
               "world→screen→world round-trips (rotated + y-flipped camera)");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
