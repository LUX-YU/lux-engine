#pragma once
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>

namespace lux::editor::workbench::detail
{
    template <class Error> auto commandFailure(const Error& error)
    {
        if constexpr (requires { error.domain; error.reason; error.message; })
            return cxx::unexpected(commands::CommandFailure{
                error.code == decltype(error.code)::BUSY ? commands::ECommandError::BUSY
                                                        : commands::ECommandError::DOMAIN_FAILURE,
                error.domain, error.reason, error.message
            });
        else
        {
            auto detail = viewFailure(error);
            return cxx::unexpected(commands::CommandFailure{
                detail.code == views::EViewFactoryError::BUSY ? commands::ECommandError::BUSY
                                                            : commands::ECommandError::DOMAIN_FAILURE,
                std::move(detail.domain), detail.domain_code, std::move(detail.detail)
            });
        }
    }
    template <const commands::CommandDescriptor& Descriptor, class Action>
    std::shared_ptr<commands::CommandEntry> bindCommand(commands::CommandEntry::Query query, Action action)
    {
        return commands::CommandEntry::bind<Descriptor>(contracts::CodeLease::builtin(), std::move(query),
            [action = std::move(action)](const commands::CommandInvocation& input) mutable
                -> commands::CommandResult<commands::DispatchReceipt> {
                auto result = action(input);
                if (!result)
                    return cxx::unexpected(result.error());
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
    }
    template <const commands::CommandDescriptor& Descriptor, const views::ViewFactoryDescriptor& Factory>
    std::shared_ptr<commands::CommandEntry> bindToolCommand(
        commands::CommandEntry::Query query, desktop::ToolOpening open
    )
    {
        return bindCommand<Descriptor>(std::move(query),
            [open = std::move(open)](const commands::CommandInvocation&) mutable {
                return open(views::ViewTypeId{Factory.type.name()});
            }
        );
    }
}
