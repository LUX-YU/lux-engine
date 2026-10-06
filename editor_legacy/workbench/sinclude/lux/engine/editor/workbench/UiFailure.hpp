#pragma once
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/engine/editor/desktop/UiError.hpp>
#include <variant>

namespace lux::editor::workbench::detail
{
    template <class Error> bool isRetryableUiFailure(const Error& error) noexcept
    {
        if constexpr (requires { error.index(); })
        {
            return std::visit([](const auto& value) { return isRetryableUiFailure(value); }, error);
        }
        else if constexpr (requires { error.cause; })
        {
            return isRetryableUiFailure(error.cause);
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
                    return isRetryableUiFailure(error.session);
                }
            }
            if constexpr (requires { error == Error::BUSY; })
            {
                return error == Error::BUSY;
            }
            else if constexpr (requires { error.code == decltype(error.code)::BUSY; })
            {
                return error.code == decltype(error.code)::BUSY;
            }
            else
            {
                return false;
            }
        }
    }
    // The host owns a small diagnostic, never a variant containing every concrete tool's error.
    template <class Error>
    desktop::UiFailure uiFailure(
        const Error& error,
        bool retryable,
        desktop::EUiError failure_code = desktop::EUiError::OPERATION_FAILURE
    )
    {
        if constexpr (requires { error.index(); })
        {
            return std::visit([&](const auto& value) { return uiFailure(value, retryable, failure_code); }, error);
        }
        else if constexpr (requires { error.cause; })
        {
            return uiFailure(error.cause, retryable, failure_code);
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
                    return uiFailure(error.session, retryable, failure_code);
                }
            }
            desktop::UiFailure result{
                retryable ? desktop::EUiError::BUSY : failure_code,
                std::string(cxx::typeToken<Error>().name()),
                0,
                {}
            };
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
            return result;
        }
    }
} // namespace lux::editor::workbench::detail
