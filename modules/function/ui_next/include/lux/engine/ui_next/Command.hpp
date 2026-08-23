#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include <lux/engine/ui_next/UiIds.hpp>

namespace lux::ui
{
    struct CommandIndex final
    {
        std::uint32_t value{(std::numeric_limits<std::uint32_t>::max)()};

        [[nodiscard]] constexpr bool isValid() const noexcept
        {
            return value != (std::numeric_limits<std::uint32_t>::max)();
        }
        [[nodiscard]] constexpr bool
        operator==(const CommandIndex&) const noexcept = default;
    };

    struct Command final
    {
        UiCommandId id;
        std::string label;
    };

    enum class ECommandDispatchResult
    {
        EXECUTED,
        DISABLED,
        NOT_FOUND
    };
} // namespace lux::ui
