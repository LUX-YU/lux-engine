#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/ComponentLoadBinding.hpp>
#include <lux/engine/ecs/transform/visibility.h>

#include <span>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_TRANSFORM_PUBLIC
    std::span<const ComponentSchema> transformComponentSchemas() noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_TRANSFORM_PUBLIC
    ComponentLoadContribution transformComponentLoadContribution() noexcept;
} // namespace lux::ecs
