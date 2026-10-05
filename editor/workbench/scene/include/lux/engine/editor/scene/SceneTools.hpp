#pragma once
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/scene/InspectorView.hpp>
#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <lux/engine/ui/Attachment.hpp>

namespace lux::ui
{
    class Root;
}
namespace lux::editor::desktop
{
    class UiRegistry;
}

namespace lux::editor::scene
{
    extern const desktop::UiDescriptor kOutlinerView;
    extern const desktop::UiDescriptor kInspectorView;
    extern const desktop::UiDescriptor kRunInspectorView;
    extern const desktop::UiDescriptor kResourceView;
    extern const desktop::UiDescriptor kSceneConfigurationView;
    enum class ESceneTool : std::uint8_t
    {
        OUTLINER,
        INSPECTOR,
        RESOURCES,
        CONFIGURATION
    };
    // Retains only the interaction group, never a Pane pointer or a live Registry borrow.
    [[nodiscard]] cxx::expected<std::shared_ptr<SceneInteractionGroup>, lux::ui::EAttachmentError>
    shareSceneInteraction(lux::ui::Root&, lux::ui::PaneHandle);
    // Prepares a detached tool through its declared factory. The source keeps its existing
    // interaction and content; the caller decides whether to transfer the candidate to Root.
    [[nodiscard]] desktop::UiResult<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>> createSceneTool(
        desktop::UiRegistry&,
        services::ServiceRegistry&,
        services::ServiceScope&,
        lux::ui::Root&,
        lux::ui::PaneHandle source,
        ESceneTool,
        lux::ui::PaneId
    );
    // Commands retain their receivers; the bound Root and RunStore must outlive the command snapshot.
    // Step/stop retain their original ticket and view-close owners at the assembly boundary.
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>>
        makeSceneToolCommands(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>(lux::ui::PaneHandle, ESceneTool)>);
    [[nodiscard]] std::vector<std::shared_ptr<commands::CommandEntry>> makeRunViewCommands(
        commands::CommandEntry::Query,
        lux::ui::Root&,
        RunStore&,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> step,
        cxx::move_only_function<commands::CommandResult<void>(RunId)> stop
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
        makePlaySceneCommand(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<StartRunId>(commands::SessionTarget)>);
} // namespace lux::editor::scene
