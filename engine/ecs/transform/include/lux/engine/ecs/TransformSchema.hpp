#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/persistence/ComponentPersistenceBinding.hpp>
#include <lux/engine/ecs/transform/visibility.h>

#include <span>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_TRANSFORM_PUBLIC
    std::span<const ComponentSchema> transformComponentSchemas() noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_TRANSFORM_PUBLIC
    ComponentPersistenceContribution transformPersistenceContribution() noexcept;
} // namespace lux::ecs
