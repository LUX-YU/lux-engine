// ============================================================================
//  active_camera_selection_test.cpp — Slice A / T2-03: the active camera is chosen
//  by the explicit ActiveCamera2DTag ONLY. Zero tagged → null_entity (a Canvas can
//  skip rendering); exactly one → that entity; more than one → null_entity
//  (ambiguous — NEVER the implicit "first camera").
//
//  Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/gameplay/2d/world/systems/Camera2DSystem.hpp>   // activeCamera
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdio>

using namespace lux::gameplay::d2;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }
}

int main()
{
    std::printf("=== active_camera_selection_test (T2-03) ===\n");

    lux::meta::EntityRegistry reg;

    // Two untagged cameras — no active camera (never the implicit first).
    const auto c1 = reg.create(); reg.emplace<Camera2DComponent>(c1);
    const auto c2 = reg.create(); reg.emplace<Camera2DComponent>(c2);
    check(activeCamera(reg) == lux::meta::null_entity,
          "two untagged cameras → no active camera (never the implicit first)");

    // Tag exactly one → that one is active.
    reg.emplace<ActiveCamera2DTag>(c2);
    check(activeCamera(reg) == c2, "the single tagged camera is the active camera");

    // Tag a second → ambiguous → null (not the first).
    reg.emplace<ActiveCamera2DTag>(c1);
    check(activeCamera(reg) == lux::meta::null_entity,
          "two tagged cameras → ambiguous → null (not resolved to the first)");

    // Remove one tag → back to a single unambiguous active camera.
    reg.remove<ActiveCamera2DTag>(c2);
    check(activeCamera(reg) == c1, "removing one tag leaves a single active camera again");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
