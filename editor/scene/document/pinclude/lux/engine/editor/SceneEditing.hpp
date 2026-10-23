#pragma once

#include <lux/engine/editor/SceneEdit.hpp>

namespace lux::editor::detail
{
    // A single structural exclusion spans prepare through commit. Returns the exact inverse only on success.
    [[nodiscard]] SceneEditResult<VSceneEdit>
    applySceneEdit(simulation::ecs::Registry&, scene::WorldResidency&, const simulation::ecs::ComponentSchemaSet&, const VSceneEdit&) noexcept;

    [[nodiscard]] SceneEditResult<SceneObjectData> captureSceneObject(
        const simulation::ecs::Registry&,
        const scene::WorldResidency&,
        const simulation::ecs::ComponentSchemaSet&,
        world::WorldObjectId
    ) noexcept;

    [[nodiscard]] SceneEditResult<std::shared_ptr<const world::WorldPartitionData>> captureScenePartition(
        const simulation::ecs::Registry&,
        const scene::WorldResidency&,
        const simulation::ecs::ComponentSchemaSet&,
        partition::PartitionOrdinal
    ) noexcept;
} // namespace lux::editor::detail
