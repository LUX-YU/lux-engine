#pragma once

#include <cstdint>
#include <type_traits>

namespace lux::math
{
    struct Extent2u
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    static_assert(std::is_standard_layout_v<Extent2u>);
    static_assert(std::is_trivially_copyable_v<Extent2u>);
    static_assert(sizeof(Extent2u) == 8);
}
