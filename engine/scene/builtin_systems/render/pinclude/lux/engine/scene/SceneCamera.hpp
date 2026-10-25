#pragma once

#include <lux/engine/render/capabilities/LitScene.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>

namespace lux::scene::detail
{
    // Cold public camera/target inputs used by effect construction. Pose/FOV, viewport and target extent remain frame
    // inputs.
    struct CameraGraphInputs final
    {
        render::TargetId target;
        rdesc::ETextureFormat format{};
        render::ETargetKind kind{};
        std::uint32_t samples{};
        bool sampled{};
        float near_distance{}, far_distance{};
        bool operator==(const CameraGraphInputs&) const noexcept = default;
    };

    [[nodiscard]] CameraGraphInputs cameraGraphInputs(const Camera&, const CameraTarget&) noexcept;

    // Stack preparation shared by this CameraMan's outputs at one Main stable point; no retained ECS borrow.
    struct SceneCamera final
    {
        Eigen::Matrix4d view;
        Eigen::Vector3d position, direction;
        float near_distance, far_distance;
    };

    [[nodiscard]] render::RenderResult<SceneCamera> prepareSceneCamera(const Camera&, const Eigen::Affine3d&) noexcept;

    // Each output supplies its own actual aspect/viewport. Returned shader values are owned.
    [[nodiscard]] render::RenderResult<render::CameraInput>
    sceneParameters(const Camera&, const SceneCamera&, const CameraTarget&) noexcept;

    [[nodiscard]] bool sameParameters(const render::CameraInput&, const render::CameraInput&) noexcept;
} // namespace lux::scene::detail
