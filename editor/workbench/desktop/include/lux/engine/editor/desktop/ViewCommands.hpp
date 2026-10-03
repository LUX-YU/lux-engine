#pragma once
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/views/ViewFactory.hpp>

namespace lux::editor::desktop
{
    class ViewHost;
    using ToolOpening = cxx::move_only_function<commands::CommandResult<views::ViewId>(views::ViewTypeId)>;

    // Caller holds the catalog publication boundary. This operation only prepares complete views
    // and adopts them through the existing Host safe point; it does not own content or factories.
    [[nodiscard]] commands::CommandResult<views::ViewId> showTool(
        ViewHost&,
        const views::ViewFactorySnapshot&,
        object::ObjectDispatcherRef,
        views::ViewTypeId
    );
    [[nodiscard]] commands::CommandResult<std::vector<std::shared_ptr<commands::CommandEntry>>> makeToolCommands(
        std::span<const std::shared_ptr<views::ViewFactoryEntry>>,
        commands::CommandEntry::Query,
        ToolOpening
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
        makeCloseViewCommand(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>(views::ViewId)>);
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
        makeAnotherViewCommand(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>(commands::SessionTarget)>);
} // namespace lux::editor::desktop
