#pragma once

#include <lux/engine/editor/scene/SceneConfigurationElement.hpp>
#include <lux/engine/editor/scene/SceneEditorCatalog.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/scene/RenderFeatureSceneBinding.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>

namespace lux::editor::scene::detail
{
    using SimulationSystems = std::shared_ptr<const simulation::SimulationSystemRegistry>;
    using SceneSystems = std::vector<lux::scene::SceneSystemRegistration>;
    using RenderFeatures = std::vector<render::RenderFeatureRegistration>;
    using RenderBindings = std::vector<lux::scene::RenderFeatureSceneBinding>;

    inline constexpr services::ServiceDependency editorDefinitions{
        services::ServiceNameView{"lux.editor.scene.editors"},
        1,
        cxx::typeToken<SceneEditorCatalog::Definition>(),
        services::EDependencyKind::DEFINITIONS,
        services::EDependencyScope::SAME,
        {},
        {},
        true
    };
    inline constexpr services::ServiceDependency configurationDependencies[]{
        {services::ServiceNameView{"lux.project.plugins"},
         1,
         cxx::typeToken<lux::project::PluginManager>(),
         services::EDependencyKind::BORROWED,
         services::EDependencyScope::ROOT},
        {services::ServiceNameView{"lux.simulation.components"},
         1,
         cxx::typeToken<simulation::ecs::ComponentSchemaSet>(),
         services::EDependencyKind::BORROWED,
         services::EDependencyScope::ROOT},
        {services::ServiceNameView{"lux.simulation.systems"},
         1,
         cxx::typeToken<SimulationSystems>(),
         services::EDependencyKind::BORROWED,
         services::EDependencyScope::ROOT},
        {services::ServiceNameView{"lux.scene.systems"},
         1,
         cxx::typeToken<SceneSystems>(),
         services::EDependencyKind::BORROWED,
         services::EDependencyScope::ROOT},
        {services::ServiceNameView{"lux.render.features"},
         1,
         cxx::typeToken<RenderFeatures>(),
         services::EDependencyKind::BORROWED,
         services::EDependencyScope::ROOT},
        {services::ServiceNameView{"lux.render.scene.bindings"},
         1,
         cxx::typeToken<RenderBindings>(),
         services::EDependencyKind::BORROWED,
         services::EDependencyScope::ROOT},
        editorDefinitions
    };
    // Factory-local resolution only; neither the resolver nor the registry escapes this call.
    [[nodiscard]] SceneConfigurationResult<SceneConfigurationInputs> resolveSceneConfigurationInputs(
        services::ServiceResolver&,
        std::size_t offset
    );
} // namespace lux::editor::scene::detail
