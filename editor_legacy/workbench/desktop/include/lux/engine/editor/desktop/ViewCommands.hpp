#pragma once
#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>

namespace lux::editor::desktop
{
    using ToolOpening = cxx::move_only_function<commands::CommandResult<lux::ui::PaneHandle>(views::ViewTypeId)>;

    // Caller holds the catalog publication boundary. Root accepts the complete unique owner.
    // This operation owns neither windows nor content and retains no second identity table.
    [[nodiscard]] commands::CommandResult<lux::ui::PaneHandle> showTool(
        lux::ui::Root&,
        UiRegistry&,
        services::ServiceScope&,
        const UiCatalog&,
        views::ViewTypeId
    );
    [[nodiscard]] commands::CommandResult<std::vector<std::shared_ptr<commands::CommandEntry>>> makeToolCommands(
        std::span<const std::shared_ptr<const UiEntry>>,
        commands::CommandEntry::Query,
        ToolOpening
    );
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
        makeCloseViewCommand(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>(lux::ui::PaneHandle)>);
    [[nodiscard]] std::shared_ptr<commands::CommandEntry>
        makeAnotherViewCommand(commands::CommandEntry::Query, cxx::move_only_function<commands::CommandResult<void>(commands::SessionTarget)>);
} // namespace lux::editor::desktop
