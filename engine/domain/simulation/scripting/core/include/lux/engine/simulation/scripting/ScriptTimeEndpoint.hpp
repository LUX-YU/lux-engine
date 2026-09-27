#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>

#include <chrono>
#include <utility>

namespace lux::simulation::script
{
    struct ScriptRealDelayEndpoint final
    {
        using StartFn = lux::script::ScriptAbilityStartResult (*)(
            void*, std::chrono::nanoseconds, lux::script::TScriptAbilityCompletion<void>
        ) noexcept;

        void* context{};
        StartFn start{};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return context != nullptr && start != nullptr;
        }

        [[nodiscard]] lux::script::ScriptAbilityStartResult invoke(
            std::chrono::nanoseconds duration,
            lux::script::TScriptAbilityCompletion<void> completion
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
