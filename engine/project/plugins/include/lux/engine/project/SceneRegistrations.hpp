#pragma once

#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>

namespace lux::project
{
    // Immutable project composition data. Each registration retains its actual callback code;
    // destroying the PluginManager does not invalidate a Scene or worker using this snapshot.
    // Render factories belong to application RenderServer startup, not this project catalog.
    struct SceneRegistrations final
    {
        simulation::ecs::ComponentSchemaSet components;
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<scene::SceneSystemRegistration> scene_systems;
    };

    [[nodiscard]] PluginResult<SceneRegistrations> readSceneRegistrations(
        std::span<const simulation::ecs::ComponentSchema> application_components = {},
        std::span<const std::shared_ptr<const PluginLibrary>> libraries = {}
    ) noexcept;
} // namespace lux::project
