/// @file transform_system_test.cpp
/// Headless test for TransformSystem hierarchy propagation (A1) + dirty
/// down-propagation (A2). Plain assert() — no external test framework.
///
/// Covers:
///   - 3-level parent→child→grandchild world composition is correct, and is
///     independent of entt's iteration order (entities created in REVERSE so
///     the view is unlikely to yield them root-first).
///   - A moving parent marks its whole subtree's WorldTransformComponent dirty,
///     even when the descendants' local transforms are unchanged.

#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/3d/Scene3D.hpp>   // d3::installSystems
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <Eigen/Geometry>
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace lux::gameplay;   // World, HierarchyComponent
using namespace lux::gameplay::d3;     // Transform / WorldTransform components + installSystems

static bool approxEqual(const Eigen::Matrix4f& a, const Eigen::Matrix4f& b, float eps = 1e-4f)
{
    return (a - b).cwiseAbs().maxCoeff() <= eps;
}

static Eigen::Matrix4f trs(const Eigen::Vector3f& p)
{
    Eigen::Affine3f t = Eigen::Affine3f::Identity();
    t.translate(p);
    t.rotate(Eigen::Quaternionf::Identity());
    t.scale(Eigen::Vector3f::Ones());
    return t.matrix();
}

int main()
{
    std::printf("=== TransformSystem hierarchy test ===\n");

    World world;
    installSystems(world);  // World is neutral now — d3 wires Transform/Camera/Animation

    // Create grandchild FIRST, root LAST, so entt's view iteration is unlikely
    // to be root→child→grandchild — exercising the memoized topological resolve.
    const auto gc = world.createEntity();
    const auto ch = world.createEntity();
    const auto rt = world.createEntity();

    {   // Root: +X 10
        auto& tc = world.emplace<TransformComponent>(rt);
        tc.position = Eigen::Vector3f(10.f, 0.f, 0.f);
        world.emplace<WorldTransformComponent>(rt);
    }
    {   // Child of root: +Y 5
        auto& tc = world.emplace<TransformComponent>(ch);
        tc.position = Eigen::Vector3f(0.f, 5.f, 0.f);
        world.emplace<WorldTransformComponent>(ch);
        world.emplace<HierarchyComponent>(ch).parent = rt;
    }
    {   // Grandchild of child: +Z 2
        auto& tc = world.emplace<TransformComponent>(gc);
        tc.position = Eigen::Vector3f(0.f, 0.f, 2.f);
        world.emplace<WorldTransformComponent>(gc);
        world.emplace<HierarchyComponent>(gc).parent = ch;
    }

    world.tick(0.016f);

    const Eigen::Matrix4f rootL  = trs({10.f, 0.f, 0.f});
    const Eigen::Matrix4f childL = trs({0.f, 5.f, 0.f});
    const Eigen::Matrix4f gcL    = trs({0.f, 0.f, 2.f});

    const Eigen::Matrix4f rtW = world.get<WorldTransformComponent>(rt).world;
    const Eigen::Matrix4f chW = world.get<WorldTransformComponent>(ch).world;
    const Eigen::Matrix4f gcW = world.get<WorldTransformComponent>(gc).world;

    assert(approxEqual(rtW, rootL) && "root world == local TRS");
    assert(approxEqual(chW, rootL * childL) && "child world == root * childLocal");
    assert(approxEqual(gcW, rootL * childL * gcL) && "grandchild world == root*child*gc (3 levels)");
    assert(std::abs(gcW(0,3) - 10.f) < 1e-4f);
    assert(std::abs(gcW(1,3) -  5.f) < 1e-4f);
    assert(std::abs(gcW(2,3) -  2.f) < 1e-4f);
    std::printf("  [OK] 3-level composition (grandchild world = %.1f, %.1f, %.1f)\n",
                gcW(0,3), gcW(1,3), gcW(2,3));

    // ---- Dirty down-propagation (A2) ----
    world.tick(0.016f);  // settle: no local changes → all dirty flags clear
    assert(!world.get<WorldTransformComponent>(gc).dirty && "grandchild clean after settle");

    world.get<TransformComponent>(rt).position = Eigen::Vector3f(20.f, 0.f, 0.f);
    world.get<TransformComponent>(rt).dirty = true;       // move ONLY the root
    world.tick(0.016f);

    assert(world.get<WorldTransformComponent>(rt).dirty && "root dirty after move");
    assert(world.get<WorldTransformComponent>(ch).dirty && "child dirty propagated from root");
    assert(world.get<WorldTransformComponent>(gc).dirty && "grandchild dirty propagated from root");
    assert(std::abs(world.get<WorldTransformComponent>(gc).world(0,3) - 20.f) < 1e-4f
           && "grandchild subtree follows the moved root");
    std::printf("  [OK] dirty propagates down + subtree follows moved root\n");

    std::printf("=== PASS ===\n");
    return 0;
}
