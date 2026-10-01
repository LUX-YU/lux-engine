#pragma once
#include <lux/engine/editor/views/CameraNavigation.hpp>
namespace lux::scene
{
    struct RayHit3D;
}
namespace lux::editor::scene
{
    [[nodiscard]] lux::editor::views::CameraNavigationResult<Eigen::Vector3d> sceneCreationPoint(
        const lux::scene::RayHit3D*,
        const lux::math::Ray3d&,
        double
    );
}
