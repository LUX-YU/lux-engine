#pragma once
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <lux/engine/editor/commands/CommandRegistry.hpp>

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
    // Commands retain their receivers; the bound Host and RunStore must outlive the command snapshot.
    // Step/stop retain their original ticket and view-close owners at the assembly boundary.
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> makeSceneToolCommands(
        commands::CommandEntry::Query,
        cxx::move_only_function<commands::CommandResult<void>(views::ViewId, ESceneTool)>
    );
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> makeRunViewCommands(
        commands::CommandEntry::Query,
        desktop::ViewHost&,
        RunStore&,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> step,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> stop
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry> makePlaySceneCommand(
        commands::CommandEntry::Query,
        cxx::move_only_function<commands::CommandResult<StartRunId>(commands::SessionTarget)>
    );
}
