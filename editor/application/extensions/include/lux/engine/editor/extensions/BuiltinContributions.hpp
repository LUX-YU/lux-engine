#pragma once
#include <lux/engine/editor/extensions/Contributions.hpp>
#include <lux/engine/editor/extensions/ExtensionCapabilities.hpp>
#include <lux/engine/editor/scene/SceneView.hpp>
#include <lux/engine/editor/scene/SceneSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialSessionFactory.hpp>
#include <lux/engine/editor/flowforge/FlowSessionFactory.hpp>
#include <lux/engine/editor/material/MaterialView.hpp>
#include <lux/engine/editor/flowforge/FlowView.hpp>

namespace lux::editor::extensions
{
    using HistoryActionLookup = cxx::move_only_function<sessions::InstalledSession*(sessions::SessionId)>;
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> builtinSessionCommands(
        SessionActivities,
        HistoryActionLookup
    );
    [[nodiscard]] std::vector<std::shared_ptr<sessions::SessionFactoryEntry>> builtinSessionFactories(
        simulation::ecs::ComponentSchemaSet,
        lux::flowforge::FlowSourceEnvironment
    );
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinSceneViewFactory(scene::SceneViewServices);
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinMaterialViewFactory(
        sessions::TSessionAccess<material::MaterialSession>,
        lux::scene::SceneRuntime&,
        material::MaterialCompilationService&,
        const scene::ProjectionEnvironment&,
        std::span<const render::RenderFeatureRegistration>,
        project::ProjectCatalogModel*
    );
    [[nodiscard]] std::shared_ptr<views::ViewFactoryEntry> builtinFlowViewFactory(flowforge::FlowViewServices);
}
