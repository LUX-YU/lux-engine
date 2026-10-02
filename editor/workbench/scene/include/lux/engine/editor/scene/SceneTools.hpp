#pragma once
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>

namespace lux::editor::desktop { class ViewHost; }

namespace lux::editor::scene
{
    enum class ESceneTool : std::uint8_t
    {
        OUTLINER,
        INSPECTOR,
        RESOURCES,
        CONFIGURATION
    };
    struct SceneToolInputs final
    {
        SceneViewServices scene;
        RunStore& runs;
        simulation::ecs::ComponentSchemaSet schemas;
        std::vector<InspectorComponent> components;
        project::ProjectCatalogModel* assets{};
        SceneConfigurationInputs configuration;
    };
    [[nodiscard]] SceneViewResult<views::DetachedView> makeRunSceneView(
        object::ObjectDispatcherRef, SceneViewServices, lux::ui::PaneId, RunId, system::SystemInstanceId
    );
    // Retains only the interaction group, never a Pane pointer or a live Registry borrow.
    [[nodiscard]] views::ViewResult<std::shared_ptr<SceneInteractionGroup>>
    shareSceneInteraction(desktop::ViewHost&, views::ViewId);
    [[nodiscard]] views::ViewFactoryResult<views::DetachedView> makeSceneToolView(
        object::ObjectDispatcherRef, lux::ui::PaneId, desktop::ViewHost&, views::ViewId, ESceneTool, SceneToolInputs
    );
}
