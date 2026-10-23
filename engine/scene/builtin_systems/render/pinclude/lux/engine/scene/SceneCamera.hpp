#pragma once

#include <lux/engine/scene/SceneEffect.hpp>
#include <lux/engine/scene/SceneViewRequest.hpp>

namespace lux::scene::detail
{
    // Called only at Main's stable Scene point. Results own all shader input values.
    [[nodiscard]] render::RenderResult<SceneParameters>
    sceneParameters(const simulation::ecs::Registry&, const SceneViewRequest&) noexcept;

    [[nodiscard]] bool sameParameters(const SceneParameters&, const SceneParameters&) noexcept;
} // namespace lux::scene::detail
