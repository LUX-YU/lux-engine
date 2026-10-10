#pragma once

#include <cstdint>
#include <cstddef>

namespace lux::ui::detail
{
    inline constexpr std::size_t FontPixelLimit = 64U * 1024U * 1024U;

    [[nodiscard]] inline bool validFontConfiguration(
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t bytes
    ) noexcept
    {
        const bool is_valid_extent = width > 0 && height > 0 && width <= 16384 && height <= 16384;
        return is_valid_extent && bytes <= FontPixelLimit && std::uint64_t(width) * height * 4U == bytes;
    }
}
