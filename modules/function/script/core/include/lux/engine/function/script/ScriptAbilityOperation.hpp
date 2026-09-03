#pragma once

#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>

namespace lux::script
{
    struct ScriptAbilityOperationError final
    {
        std::int32_t status{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return status > 0;
        }

        friend constexpr bool operator==(ScriptAbilityOperationError, ScriptAbilityOperationError) noexcept = default;
    };

    using ScriptAbilityStartResult = lux::cxx::expected<void, ScriptAbilityOperationError>;

    enum class EScriptAbilityCompletionError : std::uint8_t
    {
        STALE,
        STOPPING,
        ALREADY_COMPLETED,
        INVALID_VALUE,
        BACKPRESSURE,
        ALLOCATION_FAILURE,
    };
} // namespace lux::script
