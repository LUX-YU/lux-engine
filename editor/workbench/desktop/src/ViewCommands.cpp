#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/ui/Root.hpp>

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
            .target_type = cxx::typeToken<lux::ui::PaneHandle>()
        };
        constexpr commands::CommandDescriptor kAnotherView{
            commands::CommandIdView{"lux.editor.another-view"},
            "Another View",
            "Window",
            "",
            commands::ECommandScope::SESSION
        };
    } // namespace
    commands::CommandResult<lux::ui::PaneHandle> showTool(
        lux::ui::Root& root,
        UiRegistry& windows,
        services::ServiceScope& scope,
        const UiCatalog& factories,
        views::ViewTypeId type
    )
    {
        auto existing = windows.describe(root);
        if (!existing)
        {
            return workbench::detail::commandFailure(existing.error());
        }
        for (const auto& view : *existing)
        {
            if (view.type != type)
            {
                continue;
            }
            auto pane = root.findPane(view.handle);
            if (!pane)
            {
                return workbench::detail::commandFailure(pane.error());
            }
            (*pane)->setVisible(true);
            if (!root.requestFocus(**pane))
            {
                return cxx::unexpected(commands::CommandFailure{commands::ECommandError::BUSY, "tool.focus"});
            }
            return view.handle;
        }
        auto factory = factories.find(type.view());
        if (!factory)
        {
            return workbench::detail::commandFailure(factory.error());
        }
        auto candidate = windows.create(
            *factory,
            scope,
            {root.dispatcherRef(),
             lux::ui::PaneId{type.name()},
             {},
             {factory->descriptor().schema, {}},
             views::ViewRestoreKey{type.name()}}
        );
        if (!candidate)
        {
            return workbench::detail::commandFailure(candidate.error());
        }
        auto* pane = candidate->get();
        auto mounted = root.addSubPane(std::move(*candidate));
        if (!mounted)
        {
            return workbench::detail::commandFailure(mounted.error());
        }
        auto identity = root.identify(*pane);
        if (!identity)
        {
            return workbench::detail::commandFailure(identity.error());
        }
        return *identity;
    }
    commands::CommandResult<std::vector<std::shared_ptr<commands::CommandEntry>>> makeToolCommands(
        std::span<const std::shared_ptr<const UiEntry>> views,
        commands::CommandEntry::Query query,
        ToolOpening open
    )
    {
        const bool is_invalid_receiver = !query || !open;
        if (is_invalid_receiver)
        {
            return cxx::unexpected(commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "tool.receiver"}
            );
        }
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto receiver = std::make_shared<ToolOpening>(std::move(open));
        std::vector<std::shared_ptr<commands::CommandEntry>> result;
        for (const auto& entry : views)
        {
            if (!entry)
            {
                return cxx::unexpected(
                    commands::CommandFailure{commands::ECommandError::INVALID_ARGUMENT, "tool.factory"}
                );
            }
            const auto& descriptor = entry->descriptor();
            if (!descriptor.content_kinds.empty())
            {
                continue;
            }
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
                    {
                        return cxx::unexpected(shown.error());
                    }
                    return commands::DispatchReceipt{commands::ImmediateCompletion{}};
                }
            ));
        }
        return result;
    }
    std::shared_ptr<commands::CommandEntry> makeCloseViewCommand(
        commands::CommandEntry::Query query,
        cxx::move_only_function<commands::CommandResult<void>(lux::ui::PaneHandle)> close
    )
    {
        return workbench::detail::bindCommand<kCloseView>(
            std::move(query),
            [close = std::move(close)](const commands::CommandInvocation& input) mutable
            { return close(*input.view<lux::ui::PaneHandle>()); }
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
