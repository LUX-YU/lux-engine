#pragma once

#include <lux/engine/scene/runtime/render/visibility.h>
#include <lux/engine/simulation/ecs/ComponentSchema.hpp>

#include <span>

namespace lux::scene
{
    [[nodiscard]] LUX_ENGINE_SCENE_RUNTIME_RENDER_PUBLIC std::span<const simulation::ecs::ComponentSchema>
    sceneRenderComponentSchemas() noexcept;
} // namespace lux::scene
