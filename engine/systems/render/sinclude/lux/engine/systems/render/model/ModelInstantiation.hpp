#pragma once

#include <lux/engine/ecs/Registry.hpp>
#include <lux/engine/description/Model.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/systems/render/visibility.h>

namespace lux::engine::render
{
    // Validates the complete static model before publishing any entities. Caller owns returned entities.
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::RenderResult<std::vector<ecs::Entity>>
    createModelEntities(ecs::Registry& registry, const rdesc::ModelDescription& model) noexcept;
}
