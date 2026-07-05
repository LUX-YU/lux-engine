// ============================================================================
//  camera2d_projection_test.cpp — Slice A / Camera2D: Camera2DSystem derives an
//  orthographic view/proj from the camera's WorldTransform2D pose + its
//  units_per_view_height/aspect, auto-maintains the derived Camera2DCache (G-07),
//  and is wired by d2::install(Core). Verified by transforming known world points
//  to NDC (robust to matrix sign conventions).
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/Scene2D.hpp>
#include <lux/engine/gameplay/2d/world/components/Transform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Camera2DCacheComponent.hpp>
#include <lux/engine/gameplay/world/World.hpp>

#include <Eigen/Core>
#include <cmath>
#include <cstdio>

using lux::gameplay::World;
using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    // World point → NDC through a camera's view_proj (ortho: w stays 1).
    Eigen::Vector2f ndc(const World& w, lux::meta::entity_id cam, float wx, float wy)
    {
        const Eigen::Matrix4f& vp = w.get<Camera2DCacheComponent>(cam).view_proj;
        const Eigen::Vector4f p = vp * Eigen::Vector4f(wx, wy, 0.f, 1.f);
        return Eigen::Vector2f(p.x() / p.w(), p.y() / p.w());
    }
    bool near1(float v) { return std::abs(std::abs(v) - 1.f) < 1e-3f; }
    bool near0(float v) { return std::abs(v) < 1e-3f; }
}

int main()
{
    std::printf("=== camera2d_projection_test (Slice A) ===\n");

    World world;
    const auto inst = install(world, /*runtime=*/nullptr, D2ScenePlan{}.enableCore());
    check(inst.transform != nullptr && inst.camera != nullptr,
          "install(Core) wires both Transform2DSystem and Camera2DSystem");

    // A camera at world (5,3), FitHeight, view height 10, aspect 2 → half extents (10, 5).
    const auto cam = world.createEntity();
    world.emplace<Transform2DComponent>(cam).position = Eigen::Vector2f(5.f, 3.f);
    { auto& cc = world.emplace<Camera2DComponent>(cam); cc.units_per_view_height = 10.f; cc.aspect = 2.f; }

    check(!world.has<Camera2DCacheComponent>(cam), "before tick: no derived Camera2DCache");
    world.tick(1.f / 60.f);
    check(world.has<Camera2DCacheComponent>(cam), "after one tick: Camera2DCache auto-maintained (G-07)");

    // The camera's own world position maps to the NDC centre.
    check(near0(ndc(world, cam, 5.f, 3.f).x()) && near0(ndc(world, cam, 5.f, 3.f).y()),
          "the camera's world position maps to NDC centre (view translation correct)");

    // half_w = half_h * aspect = 5 * 2 = 10 → a point +10 in x is the horizontal edge.
    { const auto e = ndc(world, cam, 5.f + 10.f, 3.f); check(near1(e.x()) && near0(e.y()), "world +half_width in x → NDC horizontal edge"); }
    // half_h = 5 → a point +5 in y is the vertical edge.
    { const auto e = ndc(world, cam, 5.f, 3.f + 5.f); check(near0(e.x()) && near1(e.y()), "world +half_height in y → NDC vertical edge"); }

    // Zoom: doubling units_per_view_height halves the NDC extent of the same offset.
    world.get<Camera2DComponent>(cam).units_per_view_height = 20.f;   // half_h 10, half_w 20
    world.tick(1.f / 60.f);
    { const auto e = ndc(world, cam, 5.f + 10.f, 3.f); check(std::abs(std::abs(e.x()) - 0.5f) < 1e-3f, "doubling units_per_view_height halves the NDC offset (zoom out)"); }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
