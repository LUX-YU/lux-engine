#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>

#include <chrono>
#include <utility>

namespace lux::simulation::script
{
    struct ScriptRealDelayEndpoint final
    {
        void* context{};
        lux::script::ScriptAbilityStartResult (*start)(
            void*,
            std::chrono::nanoseconds,
            lux::script::ScriptAbilityCompletion<void>
        ) noexcept{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return context != nullptr && start != nullptr;
        }

        [[nodiscard]] lux::script::ScriptAbilityStartResult invoke(
            std::chrono::nanoseconds duration,
            lux::script::ScriptAbilityCompletion<void> completion
        ) const noexcept
        {
            if (!*this)
            {
                return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{4});
            }
            return start(context, duration, std::move(completion));
        }
    };
} // namespace lux::simulation::script
