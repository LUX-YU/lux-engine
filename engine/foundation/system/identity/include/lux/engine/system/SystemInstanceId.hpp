#pragma once

#include <compare>
#include <cstdint>

namespace lux::system
{
    struct SystemInstanceId final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        friend constexpr bool operator==(SystemInstanceId, SystemInstanceId) noexcept = default;
        friend constexpr auto operator<=>(SystemInstanceId, SystemInstanceId) noexcept = default;
    };
} // namespace lux::system
