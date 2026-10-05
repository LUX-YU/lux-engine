#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>

namespace lux::editor::desktop
{
    namespace
    {
        constexpr commands::CommandDescriptor kCloseView{
            .id = commands::CommandIdView{"lux.editor.close-view"},
            .label = "Close View",
            .group = "Window",
            .shortcut = "Ctrl+W",
            .scope = commands::ECommandScope::VIEW,
            .target_type = cxx::typeToken<views::ViewId>()
        };
        constexpr commands::CommandDescriptor kAnotherView{
            commands::CommandIdView{"lux.editor.another-view"},
            "Another View",
            "Window",
            "",
            commands::ECommandScope::SESSION
        };
    } // namespace
    commands::CommandResult<views::ViewId> showTool(
        ViewHost& host,
        const views::ViewFactorySnapshot& factories,
        object::ObjectDispatcherRef dispatcher,
        views::ViewTypeId type
    )
    {
        auto existing = host.describeAll();
        if (!existing)
            return workbench::detail::commandFailure(existing.error());
        for (const auto& view : *existing)
            if (view.type == type)
            {
                auto shown = host.show(view.id);
                if (!shown)
                    return workbench::detail::commandFailure(shown.error());
                auto focused = host.focus(view.id);
                if (!focused)
                    return workbench::detail::commandFailure(focused.error());
                return view.id;
            }
        views::ViewFactoryInput input{
            dispatcher,
            lux::ui::PaneId{type.name()},
            lux::object::CodeLease::builtin(),
            cxx::typeToken<std::monostate>(),
            std::make_shared<const std::monostate>()
        };
        auto candidate = factories.prepare(type, input);
        if (!candidate)
            return workbench::detail::commandFailure(candidate.error());
        auto adopted = host.adopt(*candidate, views::ViewRestoreKey{type.name()});
        if (!adopted)
            return workbench::detail::commandFailure(adopted.error());
        return adopted->id;
    }
    commands::CommandResult<std::vector<std::shared_ptr<commands::CommandEntry>>> makeToolCommands(
        std::span<const std::shared_ptr<views::ViewFactoryEntry>> views,
        commands::CommandEntry::Query query,
        ToolOpening open
    )
    {
        const bool is_invalid_receiver = !query || !open;
        if (is_invalid_receiver)
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "tool.receiver"}
            );
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto receiver = std::make_shared<ToolOpening>(std::move(open));
        std::vector<std::shared_ptr<commands::CommandEntry>> result;
        for (const auto& entry : views)
        {
            if (!entry)
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "tool.factory"}
                );
            const auto& descriptor = entry->descriptor();
            if (descriptor.binding_type != cxx::typeToken<std::monostate>())
                continue;
            result.push_back(commands::CommandEntry::create(
                lux::object::CodeLease::builtin(),
                {commands::CommandIdView{std::string("lux.editor.tool/") + std::string(descriptor.type.name())},
                 descriptor.label,
                 "Window"},
                [check](const commands::CommandQuery& input) { return (*check)(input); },
                [receiver, type = views::ViewTypeId{descriptor.type.name()}](const commands::CommandInvocation&)
                    -> commands::CommandResult<commands::DispatchReceipt>
                {
                    auto shown = (*receiver)(type);
                    if (!shown)
                        return cxx::unexpected(shown.error());
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
        }
        return result;
    }
    std::shared_ptr<commands::CommandEntry> makeCloseViewCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(views::ViewId)> close
    )
    {
        return workbench::detail::bindCommand<kCloseView>(
            std::move(query),
            [close = std::move(close)](const commands::CommandInvocation& input) mutable
            { return close(*input.view<views::ViewId>()); }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeAnotherViewCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(commands::SessionTarget)> open
    )
    {
        return workbench::detail::bindCommand<kAnotherView>(
            std::move(query),
            [open = std::move(open)](const commands::CommandInvocation& input) mutable
            { return open(std::get<commands::SessionTarget>(input.target())); }
        );
    }
} // namespace lux::editor::desktop
