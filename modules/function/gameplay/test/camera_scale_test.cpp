/// @file camera_scale_test.cpp
/// Headless test for G-08: a camera whose ancestry carries scale bakes scale into the
/// world basis, skewing the extracted view direction. CameraSystem must flag it
/// (ancestry_scale_warning) and keep the view well-formed (orthonormal rotation, not
/// magnitude-distorted). An unscaled ancestry must NOT flag.
///
/// Drives Transform + Camera systems directly. Explicit check() — NDEBUG-safe.

#include <lux/engine/gameplay/3d/world/systems/TransformSystem.hpp>
#include <lux/engine/gameplay/3d/world/systems/CameraSystem.hpp>
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/WorldTransformComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/CameraComponent.hpp>
#include <lux/engine/gameplay/world/components/HierarchyComponent.hpp>

#include <lux/engine/meta/LuxObject.hpp>
#include <Eigen/Geometry>

#include <cstdio>

using namespace lux::gameplay::d3;
using lux::gameplay::HierarchyComponent;

namespace
{
    int g_failures = 0;
    void check(bool c, const char* m) { std::printf("[%s] %s\n", c ? " ok " : "FAIL", m); if (!c) ++g_failures; }

    // A camera entity parented to `parent`, with a fixed non-trivial local rotation.
    lux::meta::entity_id makeCamera(lux::meta::EntityRegistry& reg, lux::meta::entity_id parent)
    {
        const auto e = reg.create();
        auto& tc = reg.emplace<TransformComponent>(e);
        tc.position = Eigen::Vector3f(1.f, 2.f, 3.f);
        tc.rotation = Eigen::Quaternionf(Eigen::AngleAxisf(0.6f, Eigen::Vector3f::UnitY()));
        reg.emplace<WorldTransformComponent>(e);
        reg.emplace<HierarchyComponent>(e).parent = parent;
        reg.emplace<CameraComponent>(e);
        return e;
    }
}

int main()
{
    std::printf("=== CameraScaledParentDiagnostic (G-08) ===\n");

    lux::meta::EntityRegistry reg;

    // Scaled ancestor (uniform 2x) → camera basis columns have norm 2.
    const auto scaledParent = reg.create();
    reg.emplace<TransformComponent>(scaledParent).scale = Eigen::Vector3f(2.f, 2.f, 2.f);
    reg.emplace<WorldTransformComponent>(scaledParent);
    const auto camScaled = makeCamera(reg, scaledParent);

    // Unscaled ancestor (identity) → reference.
    const auto plainParent = reg.create();
    reg.emplace<TransformComponent>(plainParent);   // scale defaults to (1,1,1)
    reg.emplace<WorldTransformComponent>(plainParent);
    const auto camPlain = makeCamera(reg, plainParent);

    TransformSystem ts;
    CameraSystem    cs;
    ts.update(reg, 0.f);
    cs.update(reg, 0.f);

    check(reg.get<CameraComponent>(camScaled).ancestry_scale_warning == true,
          "a scaled ancestor raises the camera scale diagnostic (G-08)");
    check(reg.get<CameraComponent>(camPlain).ancestry_scale_warning == false,
          "an unscaled ancestor raises no diagnostic");

    // The view stays well-formed despite the scaled ancestor: its rotation is orthonormal
    // (de-scaled basis + LookAt), so the camera is not magnitude-distorted.
    const Eigen::Matrix3f R = reg.get<CameraComponent>(camScaled).view.block<3, 3>(0, 0);
    check((R * R.transpose() - Eigen::Matrix3f::Identity()).cwiseAbs().maxCoeff() < 1e-3f,
          "the view rotation stays orthonormal under a scaled ancestor");

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nPASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
