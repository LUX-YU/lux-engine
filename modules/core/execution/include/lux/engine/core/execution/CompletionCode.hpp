#pragma once

#include <memory>
#include <stdexec/execution.hpp>

namespace lux::execution::detail
{
    // A completion driver retains the defining module until the callback has returned.
    // This environment query is independent of the scheduler driving that callback.
    struct GetCompletionCode final : stdexec::forwarding_query_t
    {
        template <class Env> [[nodiscard]] std::shared_ptr<const void> operator()(const Env& env) const noexcept
        {
            if constexpr (requires { env.query(*this); })
            {
                return env.query(*this);
            }
            else
            {
                return {};
            }
        }
    };

    inline constexpr GetCompletionCode getCompletionCode{};
} // namespace lux::execution::detail
