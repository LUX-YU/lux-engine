#pragma once

#include <cstdint>

namespace lux::ui
{
    /** Non-owning opaque presentation token. Resource lifetime belongs to the provider. */
    struct TextureHandle final
    {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return value != 0U;
        }

        [[nodiscard]] constexpr bool operator==(const TextureHandle&) const noexcept = default;
    };
} // namespace lux::ui
