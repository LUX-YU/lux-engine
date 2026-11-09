#pragma once

#include <lux/engine/editor/SceneEdit.hpp>

namespace lux::editor::detail
{
    // A single structural exclusion spans prepare through commit. Returns the exact inverse only on success.
    [[nodiscard]] SceneEditResult<VSceneEdit> applySceneEdit(
        ecs::Registry&,
        scene::WorldResidency&,
        const ecs::ComponentSchemaSet&,
        const VSceneEdit&,
        std::span<const partition::PartitionOrdinal> destinations
    ) noexcept;

    [[nodiscard]] SceneEditResult<scene::SceneObjectData> captureSceneObject(
        const ecs::Registry&,
        const scene::WorldResidency&,
        const ecs::ComponentSchemaSet&,
        world::WorldObjectId
    ) noexcept;

    [[nodiscard]] SceneEditResult<std::shared_ptr<const world::WorldPartitionData>> captureScenePartition(
        const ecs::Registry&,
        const scene::WorldResidency&,
        const ecs::ComponentSchemaSet&,
        partition::PartitionOrdinal
    ) noexcept;
} // namespace lux::editor::detail
