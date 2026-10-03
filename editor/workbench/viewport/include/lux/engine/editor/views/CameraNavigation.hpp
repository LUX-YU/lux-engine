#pragma once
#include <lux/engine/math/Ray.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <string_view>
namespace lux::editor::views
{
    struct CameraMotion final
    {
        Eigen::Vector3d local_translation{Eigen::Vector3d::Zero()};
        Eigen::Vector2d angular_delta{Eigen::Vector2d::Zero()};
        Eigen::Vector2d pan_delta{Eigen::Vector2d::Zero()};
        double dolly{};
    };

    struct ViewportCameraState final
    {
        lux::simulation::ecs::Transform3D transform;
        lux::scene::Camera camera;
    };

    // Failure text is a static diagnostic; no UI, Registry access or allocation is involved.
    template <class T> using CameraNavigationResult = lux::cxx::expected<T, std::string_view>;
    [[nodiscard]] CameraNavigationResult<ViewportCameraState>
    navigateCamera(const simulation::ecs::Transform3D&, const lux::scene::Camera&, const CameraMotion&);
    [[nodiscard]] CameraNavigationResult<lux::math::Ray3d> cameraRay(
        const simulation::ecs::WorldTransform3D&,
        const lux::scene::Camera&,
        Eigen::Vector2d,
        Eigen::Vector2d
    );
} // namespace lux::editor::views
