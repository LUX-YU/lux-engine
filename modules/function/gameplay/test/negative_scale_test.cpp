// ============================================================================
//  negative_scale_test.cpp — Slice A / T2-00: Transform2DSystem's world math is
//  well-defined for the edge cases T2-00 calls out — negative scale (mirror), zero
//  scale (degenerate but finite), and the rotation unit convention (radians, +Z CCW).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/world/systems/Transform2DSystem.hpp>
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <Eigen/Core>
#include <cmath>
#include <cstdio>

using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== negative_scale_test (T2-00) ===\n");

    lux::meta::EntityRegistry reg;
    Transform2DSystem sys;

    const auto neg  = reg.create();  reg.emplace<Transform2DComponent>(neg).scale    = Eigen::Vector2f(-1.f, 1.f);
    const auto zero = reg.create();  reg.emplace<Transform2DComponent>(zero).scale   = Eigen::Vector2f(0.f, 1.f);
    const auto rot  = reg.create();  reg.emplace<Transform2DComponent>(rot).rotation = 3.14159265f / 2.f;   // 90° CCW

    sys.update(reg, 0.f);   // auto-maintains WorldTransform2D (G-07) + composes

    // ── Negative scale → mirror (flips x, keeps y; negative 2D determinant) ──
    {
        const auto& w = reg.get<WorldTransform2DComponent>(neg).world;
        check(std::abs(w(0, 0) - (-1.f)) < 1e-5f && std::abs(w(1, 1) - 1.f) < 1e-5f,
              "negative x-scale flips the x axis, keeps y");
        check(w.block<2, 2>(0, 0).determinant() < 0.f,
              "negative scale yields a mirrored (negative-determinant) 2D basis");
    }

    // ── Zero scale → degenerate but FINITE (defined, no NaN/Inf) ──
    {
        const auto& w = reg.get<WorldTransform2DComponent>(zero).world;
        check(std::abs(w(0, 0)) < 1e-6f && std::abs(w(1, 0)) < 1e-6f, "zero x-scale collapses the x column to 0");
        check(w.allFinite(), "zero scale is degenerate but finite (no NaN/Inf)");
    }

    // ── Rotation unit: radians about +Z, CCW ──
    {
        const auto& w = reg.get<WorldTransform2DComponent>(rot).world;
        // 90° CCW: upper-left 2x2 = [[0,-1],[1,0]].
        check(std::abs(w(0, 0)) < 1e-5f && std::abs(w(1, 0) - 1.f) < 1e-5f
              && std::abs(w(0, 1) - (-1.f)) < 1e-5f && std::abs(w(1, 1)) < 1e-5f,
              "rotation is radians about +Z CCW (pi/2 -> [[0,-1],[1,0]])");
        check(std::abs(w(2, 2) - 1.f) < 1e-5f, "the embedded z axis stays identity (2D pose in a 4x4)");
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
