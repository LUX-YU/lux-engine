#pragma once

#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>
#include <lux/engine/scene/SceneParameters.hpp>

namespace lux::scene::detail
{
    // Only facts baked into the Scene recipe. Pose/FOV, viewport and target extent remain frame inputs.
    struct CameraRecipeKey final
    {
        render::TargetId target;
        rdesc::ETextureFormat format{};
        render::ETargetKind kind{};
        std::uint32_t samples{};
        bool sampled{};
        float shadow_near{}, shadow_far{};
        bool operator==(const CameraRecipeKey&) const noexcept = default;
    };

    [[nodiscard]] CameraRecipeKey cameraRecipeKey(const Camera&, const CameraTarget&, bool shadows) noexcept;

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
