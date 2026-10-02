#pragma once

#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/simulation/ecs/ComponentSchemaSet.hpp>
#include <lux/engine/simulation/SimulationSystemRegistry.hpp>
#include <lux/engine/scene/RenderScenePluginExports.hpp>
#include <lux/engine/function/render/client/RenderPluginExports.hpp>

namespace lux::project
{
    struct PluginRenderRegistrations final
    {
        std::vector<render::RenderFeatureRegistration> features;
        std::vector<lux::scene::RenderFeatureSceneBinding> bindings;
    };
    // Does not reopen the library; each returned registration retains the verified code owner.
    [[nodiscard]] PluginResult<PluginRenderRegistrations> readPluginRendering(
        const PluginLibrary&,
        std::span<const std::shared_ptr<const PluginLibrary>> dependencies = {}
    ) noexcept;
    // Complete immutable scene assembly values from already loaded runtime plugins. This does not
    // load editor extensions and remains usable by a graphical player.
    struct SceneRegistrations final
    {
        simulation::ecs::ComponentSchemaSet components;
        std::shared_ptr<const simulation::SimulationSystemRegistry> simulation_systems;
        std::vector<scene::SceneSystemRegistration> scene_systems;
        std::vector<render::RenderFeatureRegistration> features;
        std::vector<scene::RenderFeatureSceneBinding> render_bindings;
    };
    [[nodiscard]] PluginResult<SceneRegistrations> readSceneRegistrations(
        std::span<const simulation::ecs::ComponentSchema> additional = {},
        std::span<const std::shared_ptr<const PluginLibrary>> plugins = {}
    );
}
