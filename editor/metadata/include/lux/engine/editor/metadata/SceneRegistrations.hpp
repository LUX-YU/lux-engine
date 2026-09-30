#pragma once
#include <lux/engine/editor/metadata/visibility.h>
#include <lux/engine/project/PluginCatalog.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/scene/SceneSystemRegistration.hpp>
namespace lux::project
{
    class PluginLibrary;
}
namespace lux::editor
{
    struct SceneRegistrations final
    {
        lux::simulation::ecs::ComponentSchemaSet components;
        std::shared_ptr<const lux::simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<lux::scene::SceneSystemRegistration> scene_systems;
        std::vector<lux::render::RenderFeatureRegistration> features;
        std::vector<lux::scene::RenderFeatureSceneBinding> render_bindings;
    };

    [[nodiscard]] LUX_EDITOR_METADATA_PUBLIC lux::project::PluginResult<SceneRegistrations> sceneRegistrations(
        std::span<const lux::simulation::ecs::ComponentSchema> additional = {},
        std::span<const std::shared_ptr<const lux::project::PluginLibrary>> plugins = {}
    );

}
