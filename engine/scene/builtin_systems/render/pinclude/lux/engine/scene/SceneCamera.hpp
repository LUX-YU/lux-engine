#pragma once

#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/CameraTargetsComponent.hpp>
#include <lux/engine/scene/SceneParameters.hpp>

namespace lux::scene::detail
{
    // Called only at Main's stable Scene point. Results own all shader input values.
    [[nodiscard]] render::RenderResult<SceneParameters>
    sceneParameters(const Camera&, const Eigen::Affine3d&, const CameraTarget&) noexcept;

    [[nodiscard]] bool sameParameters(const SceneParameters&, const SceneParameters&) noexcept;
} // namespace lux::scene::detail
