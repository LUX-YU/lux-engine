#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/ecs/visual/visibility.h>

#include <span>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_VISUAL_PUBLIC std::span<const ComponentSchema> visualComponentSchemas(
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_VISUAL_PUBLIC ComponentSnapshotContribution
    visualComponentSnapshotContribution() noexcept;
}
