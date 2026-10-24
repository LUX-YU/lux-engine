#pragma once

#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>
#include <lux/engine/scene/SceneParameters.hpp>

namespace lux::scene::detail
{
    // Stack preparation shared by this CameraMan's outputs at one Main stable point; no retained ECS borrow.
    struct SceneCamera final
    {
        Eigen::Matrix4d view;
        Eigen::Vector3d position, direction;
        float near_distance, far_distance;
    };

    [[nodiscard]] render::RenderResult<SceneCamera> prepareSceneCamera(const Camera&, const Eigen::Affine3d&) noexcept;

    // Each output supplies its own actual aspect/viewport. Returned shader values are owned.
    [[nodiscard]] render::RenderResult<SceneParameters>
    sceneParameters(const Camera&, const SceneCamera&, const CameraTarget&) noexcept;

    [[nodiscard]] bool sameParameters(const SceneParameters&, const SceneParameters&) noexcept;
} // namespace lux::scene::detail
