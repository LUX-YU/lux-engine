#pragma once
#include <lux/engine/editor/views/ViewError.hpp>
#include <lux/cxx/compile_time/TypeToken.hpp>
#include <variant>

namespace lux::editor::workbench::detail
{
    template <class Error> bool isRetryableViewFailure(const Error& error) noexcept
    {
        if constexpr (requires { error.index(); })
            return std::visit([](const auto& value) { return isRetryableViewFailure(value); }, error);
        else if constexpr (requires { error.cause; })
            return isRetryableViewFailure(error.cause);
        else
        {
            if constexpr (requires { error.session; error.code == decltype(error.code)::SESSION; })
                if (error.code == decltype(error.code)::SESSION)
                    return isRetryableViewFailure(error.session);
            if constexpr (requires { error == Error::BUSY; })
                return error == Error::BUSY;
            else if constexpr (requires { error.code == decltype(error.code)::BUSY; })
                return error.code == decltype(error.code)::BUSY;
            else
                return false;
        }
    }
    // The host owns a small diagnostic, never a variant containing every concrete tool's error.
    template <class Error>
    views::ViewPreparationFailure viewPreparationFailure(const Error& error, bool retryable)
    {
        if constexpr (requires { error.index(); })
            return std::visit([&](const auto& value) { return viewPreparationFailure(value, retryable); }, error);
        else if constexpr (requires { error.cause; })
            return viewPreparationFailure(error.cause, retryable);
        else
        {
            if constexpr (requires { error.session; error.code == decltype(error.code)::SESSION; })
                if (error.code == decltype(error.code)::SESSION)
                    return viewPreparationFailure(error.session, retryable);
            views::ViewPreparationFailure result{std::string(cxx::typeToken<Error>().name()), 0, {}, retryable};
            if constexpr (std::is_enum_v<Error>)
                result.code = static_cast<std::uint64_t>(error);
            else if constexpr (requires { error.code; })
                result.code = static_cast<std::uint64_t>(error.code);
            if constexpr (std::is_convertible_v<Error, std::string_view>)
                result.message = std::string_view(error);
            return result;
        }
    }
}
