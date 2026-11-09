#pragma once
#include <lux/engine/ecs/ComponentSchema.hpp>
#include <span>

namespace lux::scene
{
    [[nodiscard]] std::span<const ecs::ComponentSchema> viewComponentSchemas() noexcept;
}
