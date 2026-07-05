/// @file hierarchy_cycle_test.cpp
/// Headless test for G-08: a malformed parent cycle (A→B→A) must be DETECTED and
/// broken by the TransformSystem — the update must not hang, the cycle entities keep
/// finite world matrices (a broken back-edge does not recurse), and cyclesLastUpdate()
/// reports it. A well-formed hierarchy reports zero cycles.
///
/// Drives the TransformSystem directly (so cyclesLastUpdate() is observable).
/// Explicit check() — meaningful under NDEBUG too.

#include <lux/engine/gameplay/3d/world/systems/TransformSystem.hpp>
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>

using lux::gameplay::d3::TransformSystem;
using lux::gameplay::d3::TransformComponent;
using lux::gameplay::d3::WorldTransformComponent;
using lux::gameplay::HierarchyComponent;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    lux::meta::entity_id makeXform(lux::meta::EntityRegistry& reg)
    {
        const auto e = reg.create();
        reg.emplace<TransformComponent>(e);
        reg.emplace<WorldTransformComponent>(e);
        return e;
    }
}

int main()
{
    std::printf("=== HierarchyCycleRejected (G-08) ===\n");

    // Well-formed: B parented to A (a two-node forest). No cycle.
    {
        lux::meta::EntityRegistry reg;
        const auto a = makeXform(reg);
        const auto b = makeXform(reg);
        reg.emplace<HierarchyComponent>(b).parent = a;

        TransformSystem sys;
        sys.update(reg, 0.f);
        check(sys.cyclesLastUpdate() == 0, "a well-formed hierarchy reports zero cycles");
    }

    // Malformed: A→B→A parent cycle. Must not hang; must be detected + broken.
    {
        lux::meta::EntityRegistry reg;
        const auto a = makeXform(reg);
        const auto b = makeXform(reg);
        // Non-identity locals so b*a != Identity — otherwise the stale/fresh mixing in the
        // old code produces no observable drift (Identity is a fixed point either way).
        reg.get<TransformComponent>(a).position = Eigen::Vector3f(1.f, 0.f, 0.f);
        reg.get<TransformComponent>(b).position = Eigen::Vector3f(0.f, 2.f, 0.f);
        reg.emplace<HierarchyComponent>(a).parent = b;
        reg.emplace<HierarchyComponent>(b).parent = a;

        TransformSystem sys;
        sys.update(reg, 0.f);   // must terminate (no infinite recursion)
        check(sys.cyclesLastUpdate() > 0, "a parent cycle is detected + broken");
        check(reg.get<WorldTransformComponent>(a).world.allFinite()
              && reg.get<WorldTransformComponent>(b).world.allFinite(),
              "cycle entities keep finite world matrices (no garbage)");

        // P1-1: with unchanged locals the cycle nodes must reach a STABLE fixed point —
        // no cross-frame drift (the old code mixed a stale/fresh sibling matrix so the
        // world grew by (b*a) every frame).
        const Eigen::Matrix4f a1 = reg.get<WorldTransformComponent>(a).world;
        const Eigen::Matrix4f b1 = reg.get<WorldTransformComponent>(b).world;
        sys.update(reg, 0.f);   // second frame, nothing changed
        const bool a_stable = (reg.get<WorldTransformComponent>(a).world - a1).cwiseAbs().maxCoeff() == 0.0f;
        const bool b_stable = (reg.get<WorldTransformComponent>(b).world - b1).cwiseAbs().maxCoeff() == 0.0f;
        check(a_stable && b_stable,
              "cycle world matrices are stable across frames (no drift, P1-1)");

        // Breaking the cycle (A becomes a root) clears the count next update.
        reg.remove<HierarchyComponent>(a);
        sys.update(reg, 0.f);
        check(sys.cyclesLastUpdate() == 0, "removing the back-edge clears the cycle count");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
