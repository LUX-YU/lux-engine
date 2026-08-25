#pragma once

#include <lux/engine/ecs/ComponentSchema.hpp>
#include <lux/engine/ecs/ComponentLoadBinding.hpp>
#include <lux/engine/ecs/ComponentSnapshotBinding.hpp>
#include <lux/engine/ecs/hierarchy/visibility.h>

#include <span>

namespace lux::ecs
{
    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    std::span<const ComponentSchema> hierarchyComponentSchemas();

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    ComponentLoadContribution hierarchyComponentLoadContribution() noexcept;

    [[nodiscard]] LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    ComponentSnapshotContribution
    hierarchyComponentSnapshotContribution() noexcept;
} // namespace lux::ecs
