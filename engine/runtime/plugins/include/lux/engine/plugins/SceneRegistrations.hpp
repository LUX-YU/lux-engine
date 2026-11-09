#pragma once

#include <lux/engine/plugins/PluginLibrary.hpp>
#include <lux/engine/scene/SceneContentSelection.hpp>
#include <lux/engine/ecs/ComponentSchemaSet.hpp>

namespace lux::plugins
{
    // Immutable project composition data. Each registration retains its actual callback code;
    // destroying the PluginManager does not invalidate a Scene or worker using this snapshot.
    // Render factories belong to application RenderServer startup, not this project catalog.
    struct SceneRegistrations final
    {
        ecs::ComponentSchemaSet components;
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<scene::SceneSystemRegistration> scene_systems;
        std::vector<scene::GeometryDeclaration> geometries;
        std::vector<world::PartitionDeclaration> partitions;
        scene::SceneContentAllowance allowed_content;
    };

    [[nodiscard]] PluginResult<SceneRegistrations> readSceneRegistrations(
        std::span<const ecs::ComponentSchema> application_components = {},
        std::span<const std::shared_ptr<const PluginLibrary>> libraries = {},
        const scene::SceneContentAllowance* allowed = nullptr
    ) noexcept;
} // namespace lux::plugins
