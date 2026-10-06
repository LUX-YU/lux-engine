#pragma once
#include <algorithm>
#include <lux/engine/editor/desktop/ViewCommands.hpp>
#include <variant>

namespace lux::editor::workbench::detail
{
    template <class Error> auto commandFailure(const Error& error)
    {
        if constexpr (requires {
                          error.domain;
                          error.reason;
                          error.message;
                      })
        {
            return cxx::unexpected(commands::CommandFailure{
                error.code == decltype(error.code)::BUSY ? commands::ECommandError::BUSY
                                                         : commands::ECommandError::DOMAIN_FAILURE,
                error.domain,
                error.reason,
                error.message
            });
        }
        else if constexpr (requires {
                               error.domain;
                               error.domain_code;
                               error.detail;
                           })
        {
            return cxx::unexpected(commands::CommandFailure{
                error.code == decltype(error.code)::BUSY ? commands::ECommandError::BUSY
                                                         : commands::ECommandError::DOMAIN_FAILURE,
                error.domain,
                error.domain_code,
                error.detail
            });
        }
        else if constexpr (requires { error.cause; })
        {
            return commandFailure(error.cause);
        }
        else if constexpr (requires { error.index(); })
        {
            return std::visit([](const auto& value) { return commandFailure(value); }, error);
        }
        else
        {
            if constexpr (requires {
                              error.session;
                              error.code == decltype(error.code)::SESSION;
                          })
            {
                if (error.code == decltype(error.code)::SESSION)
                {
                    return commandFailure(error.session);
                }
            }
            commands::CommandFailure result{
                commands::ECommandError::DOMAIN_FAILURE,
                std::string(cxx::typeToken<Error>().name())
            };
            if constexpr (requires { error.retryable; })
            {
                if (error.retryable)
                {
                    result.code = commands::ECommandError::BUSY;
                }
            }
            if constexpr (requires { error == Error::BUSY; })
            {
                if (error == Error::BUSY)
                {
                    result.code = commands::ECommandError::BUSY;
                }
            }
            if constexpr (std::is_enum_v<Error>)
            {
                result.domain_code = static_cast<std::uint64_t>(error);
            }
            else if constexpr (requires { error.code; })
            {
                result.domain_code = static_cast<std::uint64_t>(error.code);
            }
            if constexpr (std::is_convertible_v<Error, std::string_view>)
            {
                result.detail = std::string_view(error);
            }
            else if constexpr (requires { std::string{error.message}; })
            {
                result.detail = error.message;
            }
            else if constexpr (requires {
                                   error.message.begin();
                                   error.message.end();
                               })
            {
                const auto end = std::find(error.message.begin(), error.message.end(), '\0');
                result.detail.assign(error.message.begin(), end);
            }
            return cxx::unexpected(std::move(result));
        }
    }

    template <const commands::CommandDescriptor& Descriptor, class Action>
    std::shared_ptr<commands::CommandEntry> bindCommand(commands::CommandEntry::Query query, Action action)
    {
        return commands::CommandEntry::bind<Descriptor>(
            lux::object::CodeLease::builtin(),
            std::move(query),
            [action = std::move(action)](const commands::CommandInvocation& input
            ) mutable -> commands::CommandResult<commands::DispatchReceipt>
            {
                auto result = action(input);
                if (!result)
                {
                    return cxx::unexpected(result.error());
                }
                return commands::DispatchReceipt{commands::ImmediateCompletion{}};
            }
        );
    }
    template <const commands::CommandDescriptor& Descriptor, auto& Factory>
    std::shared_ptr<commands::CommandEntry> bindToolCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return bindCommand<Descriptor>(
            std::move(query),
            [open = std::move(open)](const commands::CommandInvocation&) mutable
            { return open(views::ViewTypeId{Factory.type.name()}); }
        );
    }
} // namespace lux::editor::workbench::detail
