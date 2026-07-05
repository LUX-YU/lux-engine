// ============================================================================
//  transform2d_hierarchy_test.cpp — Slice A / T2-00+T2-01: Transform2DSystem
//  composes local 2D TRS → world through the neutral HierarchyComponent, auto-
//  maintains the derived WorldTransform2D (G-07, no caller back-fill), and is
//  wired by d2::install(Core). Local→world propagation matches 3D semantics.
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>
#include <lux/engine/gameplay/world/World.hpp>
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <Eigen/Core>
#include <cmath>
#include <cstdio>

using lux::gameplay::World;
using lux::gameplay::HierarchyComponent;
using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    Eigen::Vector2f worldPos(const World& w, lux::meta::entity_id e)
    { return w.get<WorldTransform2DComponent>(e).world.block<2, 1>(0, 3); }

    bool near2(const Eigen::Vector2f& a, float x, float y) { return (a - Eigen::Vector2f(x, y)).norm() < 1e-4f; }
}

int main()
{
    std::printf("=== transform2d_hierarchy_test (T2-00/T2-01) ===\n");

    World world;
    const auto inst = install(world, /*runtime=*/nullptr, D2ScenePlan{}.enableCore());
    check(inst.transform != nullptr, "install(Core) wires a Transform2DSystem");

    // Create grandchild FIRST, root LAST (reverse) to exercise the memoized topological
    // resolve. Each has ONLY a local Transform2D — WorldTransform2D is auto-maintained.
    const auto gc = world.createEntity();
    const auto ch = world.createEntity();
    const auto rt = world.createEntity();

    world.emplace<Transform2DComponent>(rt).position = Eigen::Vector2f(10.f, 0.f);
    { auto& c = world.emplace<Transform2DComponent>(ch); c.position = Eigen::Vector2f(0.f, 5.f);
      world.emplace<HierarchyComponent>(ch).parent = rt; }
    { auto& c = world.emplace<Transform2DComponent>(gc); c.position = Eigen::Vector2f(2.f, 0.f);
      world.emplace<HierarchyComponent>(gc).parent = ch; }

    check(!world.has<WorldTransform2DComponent>(rt), "before tick: no derived WorldTransform2D");

    world.tick(1.f / 60.f);

    check(world.has<WorldTransform2DComponent>(rt)
          && world.has<WorldTransform2DComponent>(ch)
          && world.has<WorldTransform2DComponent>(gc),
          "after one tick: WorldTransform2D auto-maintained on every Transform2D entity (G-07)");

    check(near2(worldPos(world, rt), 10.f, 0.f), "root world = (10, 0)");
    check(near2(worldPos(world, ch), 10.f, 5.f), "child world = parent + local = (10, 5)");
    check(near2(worldPos(world, gc), 12.f, 5.f), "grandchild world = (12, 5) — 3-level propagation");

    // A rotated parent rotates the child's world offset (2D semantics, +Z CCW).
    world.get<Transform2DComponent>(rt).rotation = 3.14159265f / 2.f;   // 90° CCW
    world.get<Transform2DComponent>(rt).dirty = true;
    world.tick(1.f / 60.f);
    // child local (0,5): rotated 90° CCW → (-5, 0); + root translation (10,0) → (5, 0).
    check(near2(worldPos(world, ch), 5.f, 0.f), "a rotated parent rotates the child's world offset (90° CCW)");

    // Removing the source Transform2D drops the derived WorldTransform2D (G-07 symmetry).
    world.registry().remove<Transform2DComponent>(gc);
    world.tick(1.f / 60.f);
    check(!world.has<WorldTransform2DComponent>(gc), "removing Transform2D drops its derived WorldTransform2D");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
