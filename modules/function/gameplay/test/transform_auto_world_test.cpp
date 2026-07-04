/// @file transform_auto_world_test.cpp
/// Headless test for G-07: the TransformSystem OWNS the derived WorldTransform
/// invariant. A loader / editor / game creates only the local TransformComponent
/// (WorldTransform is derived + non-persistent); after ONE tick the system must have
/// auto-emplaced AND composed a valid WorldTransform — no caller back-fill. Removing
/// the source Transform must drop the derived WorldTransform (no orphan).
///
/// Explicit check() (not assert) so the test is meaningful under NDEBUG too.

#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/3d/Scene3D.hpp>   // d3::installSystems
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>

#include <Eigen/Geometry>
#include <cstdio>

using namespace lux::gameplay;       // World
using namespace lux::gameplay::d3;   // Transform / WorldTransform + installSystems

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== TransformAutoMaintainsWorldTransform (G-07) ===\n");

    World world;
    installSystems(world);

    // A loader-shaped entity: ONLY a local Transform, NO WorldTransform.
    const auto e = world.createEntity();
    auto& tc = world.emplace<TransformComponent>(e);
    tc.position = Eigen::Vector3f(3.f, 4.f, 5.f);

    check(!world.registry().all_of<WorldTransformComponent>(e),
          "before tick: a Transform entity has no WorldTransform");

    world.tick(0.f);   // TransformSystem auto-emplaces + composes (G-07)

    check(world.registry().all_of<WorldTransformComponent>(e),
          "after one tick: WorldTransform is auto-maintained (no caller back-fill)");
    if (world.registry().all_of<WorldTransformComponent>(e))
    {
        const Eigen::Vector3f t = world.get<WorldTransformComponent>(e).world.block<3, 1>(0, 3);
        check((t - Eigen::Vector3f(3.f, 4.f, 5.f)).norm() < 1e-5f,
              "the auto-maintained world matrix matches the local TRS");
    }

    // Removing the source Transform must drop the derived WorldTransform (no orphan).
    world.registry().remove<TransformComponent>(e);
    world.tick(0.f);
    check(!world.registry().all_of<WorldTransformComponent>(e),
          "removing the source Transform drops the derived WorldTransform");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
